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
endif()

juce_add_console_app(taw PRODUCT_NAME "taw")

target_sources(taw PRIVATE
    src/main.cpp
    src/App.cpp
    src/Session.cpp
    src/Commands.cpp
    src/AudioEngine.cpp
    src/Bounce.cpp
    src/Import.cpp
    src/Safety.cpp
    src/Editing.cpp
    src/Instruments.cpp
    src/Routing.cpp
    src/Inserts.cpp
    src/TrackMenu.cpp
    src/Midi.cpp
    src/MidiEditor.cpp
    src/Plugins.cpp
    src/Ui.cpp)

target_compile_definitions(taw PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_PLUGINHOST_VST3=1 # instrument tracks host VST3 plugins (in process for now)
    TAW_VERSION="${PROJECT_VERSION}")

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
    target_sources(taw PRIVATE src/TerminalSetup.cpp)
endif()

if (WIN32)
    target_compile_definitions(taw PRIVATE JUCE_ASIO=1 JUCE_WASAPI=1 JUCE_DIRECTSOUND=1 NOMINMAX
        JUCE_USE_WINDOWS_MEDIA_FORMAT=1) # import m4a/aac/wma through Windows
    target_include_directories(taw PRIVATE ${asiosdk_SOURCE_DIR}/common)
endif()

target_link_libraries(taw PRIVATE
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_data_structures
    juce::juce_events
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
