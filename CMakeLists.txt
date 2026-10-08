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
    src/Instruments.cpp
    src/Routing.cpp
    src/Inserts.cpp
    src/AlsImport.cpp
    src/AutomationEdit.cpp
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

target_link_libraries(taw PRIVATE
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
