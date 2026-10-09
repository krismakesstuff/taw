cmake_minimum_required(VERSION 3.24)
project(taw VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        9.0.3
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(juce)

# Steinberg ASIO SDK, used under its GPLv3 option (compatible with taw's AGPLv3).
if (WIN32)
    FetchContent_Declare(asiosdk
        URL      https://download.steinberg.net/sdk_downloads/ASIO-SDK_2.3.4_2025-10-15.zip
        URL_HASH SHA256=D5EBF0C20DD2C5F43771FD0C1418F4B361BF52434EE670097CFA6B3A335E2ECA)
    FetchContent_MakeAvailable(asiosdk)

    # FFmpeg's libraries (Gyan's shared GPL build, compatible with taw's AGPLv3): video decoding
    # for the picture window.
    FetchContent_Declare(ffmpeg
        URL      https://github.com/GyanD/codexffmpeg/releases/download/9.0.2/ffmpeg-9.0.2-full_build-shared.zip
        URL_HASH SHA256=8D31E162F1616E37AAB3FA2DB991B97E1B8DBEB1C8465FD81A81BE16FF91B328)
    FetchContent_MakeAvailable(ffmpeg)
endif()

juce_add_console_app(taw PRODUCT_NAME "taw")

target_sources(taw PRIVATE
    src/main.cpp
    src/App.cpp
    src/Session.cpp
    src/Commands.cpp
    src/AudioEditor.cpp
    src/AudioEngine.cpp
    src/Bounce.cpp
    src/Browser.cpp
    src/Import.cpp
    src/Library.cpp
    src/LiveKeys.cpp
    src/Timing.cpp
    src/Safety.cpp
    src/Editing.cpp
    src/Playlists.cpp
    src/Instruments.cpp
    src/Routing.cpp
    src/Inserts.cpp
    src/AlsImport.cpp
    src/SessionScreen.cpp
    src/RenderPool.cpp
    src/SystemStats.cpp
    src/AutomationEdit.cpp
    src/HubPanel.cpp
    src/TrackMenu.cpp
    src/Midi.cpp
    src/MidiEditor.cpp
    src/Plugins.cpp
    src/Ui.cpp
    src/Video.cpp)

target_compile_definitions(taw PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_PLUGINHOST_VST3=1 # instrument tracks host VST3 plugins (in process for now)
    TAW_VERSION="${PROJECT_VERSION}")

if (APPLE)
    target_compile_definitions(taw PRIVATE JUCE_PLUGINHOST_AU=1) # and Audio Units on macOS
    target_link_libraries(taw PRIVATE "-framework CoreAudioKit")  # their generic editor (AUGenericView)
endif()

# Which build this is (commit, + for changes on top, when), shown in the title bar.
set(TAW_BUILD_INFO ${CMAKE_BINARY_DIR}/generated/BuildInfo.cpp)
add_custom_target(taw_build_info
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${TAW_BUILD_INFO} -P ${CMAKE_SOURCE_DIR}/cmake/BuildInfo.cmake
    BYPRODUCTS ${TAW_BUILD_INFO})
add_dependencies(taw taw_build_info)
target_sources(taw PRIVATE ${TAW_BUILD_INFO})

if (WIN32)
    target_sources(taw PRIVATE src/ConsoleWindows.cpp)
else()
    target_sources(taw PRIVATE src/ConsolePosix.cpp)
endif()

if (APPLE)
    target_sources(taw PRIVATE src/TerminalSetup.cpp src/MacFocus.mm)
endif()

# The audio hub (docs/plan-audio-hub.md): macOS only so far. Off builds taw as it is on
# Windows, without it (the hub command and page say so).
option(TAW_WITH_HUB "Build the audio hub (macOS only so far)" ${APPLE})

if (APPLE AND TAW_WITH_HUB)
    # The hub's virtual devices: taw.driver, a CoreAudio plug-in ("taw 1" to "taw 4", and the
    # hidden "taw hub"), built beside taw; taw --install-driver puts it where coreaudiod looks.
    add_library(taw_driver MODULE driver/TawDriver.c)
    configure_file(driver/Info.plist ${CMAKE_BINARY_DIR}/generated/DriverInfo.plist @ONLY)
    set_target_properties(taw_driver PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION driver
        OUTPUT_NAME taw
        C_STANDARD 11
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/taw_artefacts/$<CONFIG>)
    target_compile_options(taw_driver PRIVATE -Wall -Wextra)
    target_link_libraries(taw_driver PRIVATE "-framework CoreAudio" "-framework CoreFoundation")
    add_custom_command(TARGET taw_driver POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/generated/DriverInfo.plist $<TARGET_BUNDLE_CONTENT_DIR:taw_driver>/Info.plist
        COMMAND codesign --force --sign - $<TARGET_BUNDLE_DIR:taw_driver>
        VERBATIM)
    add_dependencies(taw taw_driver)

    # The hub itself; its shared memory is POSIX.
    target_sources(taw PRIVATE src/Hub.cpp src/HubDevice.cpp src/HubServer.cpp src/HubShared.cpp src/HubMac.mm)
    target_link_libraries(taw PRIVATE "-framework AVFoundation")
    target_compile_definitions(taw PRIVATE TAW_HUB=1)

    # The hub runs from "taw hub.app" beside taw: the same binary in a bundle of its own, so
    # macOS asks about the microphone for it (it runs from launchd, not a terminal).
    configure_file(cmake/HubInfo.plist ${CMAKE_BINARY_DIR}/generated/HubInfo.plist @ONLY)
    set(TAW_HUB_APP "$<TARGET_FILE_DIR:taw>/taw hub.app")
    add_custom_command(TARGET taw POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${TAW_HUB_APP}/Contents/MacOS"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/generated/HubInfo.plist "${TAW_HUB_APP}/Contents/Info.plist"
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:taw> "${TAW_HUB_APP}/Contents/MacOS/taw"
        COMMAND codesign --force --sign - "${TAW_HUB_APP}"
        VERBATIM)
endif()

if (WIN32)
    target_compile_definitions(taw PRIVATE JUCE_ASIO=1 JUCE_WASAPI=1 JUCE_DIRECTSOUND=1 NOMINMAX
        JUCE_USE_WINDOWS_MEDIA_FORMAT=1) # import m4a/aac/wma through Windows
    target_include_directories(taw PRIVATE ${asiosdk_SOURCE_DIR}/common)

    # Delay-loaded: taw starts (and loads ~130 MB less) until a video is opened, and without the
    # DLLs it still runs, just without video. They're copied next to taw.exe, with FFmpeg's licence.
    set(TAW_FFMPEG_LIBS avformat avcodec avutil swscale)
    set(TAW_FFMPEG_DLLS avformat-63 avcodec-63 avutil-61 swscale-10)
    target_include_directories(taw PRIVATE ${ffmpeg_SOURCE_DIR}/include)
    foreach(lib ${TAW_FFMPEG_LIBS})
        target_link_libraries(taw PRIVATE ${ffmpeg_SOURCE_DIR}/lib/${lib}.lib)
    endforeach()
    target_link_libraries(taw PRIVATE delayimp)
    foreach(dll ${TAW_FFMPEG_DLLS})
        target_link_options(taw PRIVATE /DELAYLOAD:${dll}.dll)
    endforeach()
    foreach(dll ${TAW_FFMPEG_DLLS} swresample-7) # avcodec needs swresample
        add_custom_command(TARGET taw POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ffmpeg_SOURCE_DIR}/bin/${dll}.dll $<TARGET_FILE_DIR:taw>)
    endforeach()
    add_custom_command(TARGET taw POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ffmpeg_SOURCE_DIR}/LICENSE $<TARGET_FILE_DIR:taw>/FFMPEG-LICENSE.txt)
    target_compile_definitions(taw PRIVATE TAW_VIDEO=1)
endif()

if (APPLE)
    # FFmpeg's libraries from Homebrew (brew install ffmpeg). Weak-linked: without them taw still
    # starts, just without video. Without them at build time, video is left out.
    find_path(TAW_FFMPEG_INCLUDE libavformat/avformat.h
        HINTS /opt/homebrew/opt/ffmpeg/include /usr/local/opt/ffmpeg/include)
    set(TAW_FFMPEG_FOUND ${TAW_FFMPEG_INCLUDE})
    foreach(lib avformat avcodec avutil swscale)
        find_library(TAW_FFMPEG_${lib} ${lib} HINTS /opt/homebrew/opt/ffmpeg/lib /usr/local/opt/ffmpeg/lib)
        if (NOT TAW_FFMPEG_${lib})
            set(TAW_FFMPEG_FOUND "")
        endif()
    endforeach()

    if (TAW_FFMPEG_FOUND)
        target_include_directories(taw PRIVATE ${TAW_FFMPEG_INCLUDE})
        foreach(lib avformat avcodec avutil swscale)
            target_link_options(taw PRIVATE "LINKER:-weak_library,${TAW_FFMPEG_${lib}}")
        endforeach()
        target_compile_definitions(taw PRIVATE TAW_VIDEO=1)
    else()
        message(STATUS "FFmpeg not found (brew install ffmpeg): building without video")
    endif()
endif()

target_link_libraries(taw PRIVATE
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
