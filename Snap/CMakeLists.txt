cmake_minimum_required(VERSION 3.22)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal macOS binary")
endif()

# Bump on every build that is handed out: Logic caches Audio Units by version.
project(Snap VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
    JUCE
    URL https://github.com/juce-framework/JUCE/archive/refs/tags/8.0.15.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(JUCE)

set(SNAP_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND SNAP_FORMATS AU)
endif()

# FROZEN IDENTIFIERS - never change them, or hosts lose the plug-in and saved sessions:
# AU aufx / PLUGIN_CODE Snap / PLUGIN_MANUFACTURER_CODE Dwks / BUNDLE_ID com.dripworks.snap.
juce_add_plugin(Snap
    COMPANY_NAME "Dripworks"
    COMPANY_COPYRIGHT "Dripworks"
    BUNDLE_ID com.dripworks.snap
    PLUGIN_MANUFACTURER_CODE Dwks
    PLUGIN_CODE Snap
    PLUGIN_NAME "Snap"
    PRODUCT_NAME "Snap"
    DESCRIPTION "Drum transient shaper"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    FORMATS ${SNAP_FORMATS}
    AU_MAIN_TYPE kAudioUnitType_Effect
    VST3_CATEGORIES Fx Dynamics
)

target_sources(Snap PRIVATE Source/Snap.cpp)
juce_add_binary_data(SnapFonts SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/Assets/Fonts/GeistMono-Medium.ttf)

target_compile_definitions(Snap
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_REPORT_APP_USAGE=0
)

target_link_libraries(Snap
    PRIVATE
        SnapFonts
        juce::juce_audio_utils
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# One test executable that drives the real processor and editor without a host.
# `SnapTests --screenshot <file.png>` renders the editor instead of running the tests.
add_executable(SnapTests Tests/SnapTests.cpp)
target_include_directories(SnapTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
target_link_libraries(SnapTests
    PRIVATE
        Snap
        juce::juce_audio_utils
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
# The tests compare floats bit-exactly on purpose and replace the global operator new.
if(NOT MSVC)
    set_source_files_properties(Tests/SnapTests.cpp PROPERTIES COMPILE_OPTIONS "-Wno-float-equal;-Wno-mismatched-new-delete")
endif()
set_target_properties(SnapTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
