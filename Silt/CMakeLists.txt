cmake_minimum_required(VERSION 3.22)

if(WIN32)
    message(FATAL_ERROR "silt targets macOS and Linux")
endif()

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal macOS binary")
endif()

# Bump on every build that is handed out: Logic caches Audio Units by version.
project(Silt VERSION 0.1.0 LANGUAGES C CXX)

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

set(SILT_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND SILT_FORMATS AU)
endif()

# FROZEN IDENTIFIERS - never change them, or hosts lose the plug-in and saved sessions:
# AU aumf (MIDI-controlled effect, so Logic can route a MIDI track into it) / PLUGIN_CODE Silt /
# PLUGIN_MANUFACTURER_CODE Dwks / BUNDLE_ID com.dripworks.silt / COMPANY_NAME daveydidit / PLUGIN_NAME / PRODUCT_NAME silt.
juce_add_plugin(Silt
    COMPANY_NAME "daveydidit"
    COMPANY_COPYRIGHT "daveydidit"
    BUNDLE_ID com.dripworks.silt
    PLUGIN_MANUFACTURER_CODE Dwks
    PLUGIN_CODE Silt
    PLUGIN_NAME "silt"
    PRODUCT_NAME "silt"
    DESCRIPTION "Granular effect"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    FORMATS ${SILT_FORMATS}
    AU_MAIN_TYPE kAudioUnitType_MusicEffect
    VST3_CATEGORIES Fx
)

target_sources(Silt PRIVATE Source/Silt.cpp Source/SiltEditor.cpp)

# Geist Mono (SIL OFL 1.1, Assets/Fonts/OFL.txt), the same faces as crumb.
juce_add_binary_data(SiltFonts SOURCES Assets/Fonts/GeistMono-Regular.ttf Assets/Fonts/GeistMono-SemiBold.ttf)
set_target_properties(SiltFonts PROPERTIES POSITION_INDEPENDENT_CODE TRUE)

target_compile_definitions(Silt
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_FORCE_USE_LEGACY_PARAM_IDS=0
        JUCE_REPORT_APP_USAGE=0
)

target_link_libraries(Silt
    PRIVATE
        SiltFonts
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# One test executable that drives the real processor and editor without a host.
add_executable(SiltTests Tests/SiltTests.cpp)
target_include_directories(SiltTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
target_link_libraries(SiltTests
    PRIVATE
        SiltFonts
        juce::juce_cryptography
        Silt
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
# The tests compare floats bit-exactly on purpose and replace the global operator new.
if(NOT MSVC)
    set_source_files_properties(Tests/SiltTests.cpp PROPERTIES COMPILE_OPTIONS "-Wno-float-equal;-Wno-mismatched-new-delete")
endif()
set_target_properties(SiltTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_compile_definitions(SiltTests PRIVATE JUCE_MODAL_LOOPS_PERMITTED=1) # public message pump for the editor tests

target_compile_definitions(SiltTests PRIVATE SILT_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Tests" SILT_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
