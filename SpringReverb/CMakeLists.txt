cmake_minimum_required(VERSION 3.22)

project(SpringTank VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
FetchContent_Declare(
    JUCE
    URL https://github.com/juce-framework/JUCE/archive/refs/tags/8.0.15.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(JUCE)

# AAX is configured here so a Pro Tools build is a switch, not a fork.
# The Avid AAX SDK is not redistributable; point SPRINGTANK_AAX_SDK_PATH at a
# local checkout (the folder that contains Interfaces/ACF). AAX itself builds
# on macOS and Windows only.
option(SPRINGTANK_BUILD_AAX "Add the AAX target (requires the Avid AAX SDK)" OFF)
set(SPRINGTANK_AAX_SDK_PATH "" CACHE PATH "Path to the Avid AAX SDK")
option(SPRINGTANK_COPY_AFTER_BUILD "Copy plugin binaries into the user plug-in folders" OFF)

set(SPRINGTANK_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND SPRINGTANK_FORMATS AU)
endif()

if(SPRINGTANK_BUILD_AAX)
    if(NOT APPLE AND NOT WIN32)
        message(FATAL_ERROR "AAX builds are available on macOS and Windows only.")
    endif()
    if(SPRINGTANK_AAX_SDK_PATH STREQUAL "")
        message(FATAL_ERROR
            "SPRINGTANK_BUILD_AAX=ON requires -DSPRINGTANK_AAX_SDK_PATH=/path/to/aax-sdk")
    endif()
    juce_set_aax_sdk_path("${SPRINGTANK_AAX_SDK_PATH}")
    list(APPEND SPRINGTANK_FORMATS AAX)
endif()

if(SPRINGTANK_COPY_AFTER_BUILD)
    set_property(GLOBAL PROPERTY JUCE_COPY_PLUGIN_AFTER_BUILD TRUE)
endif()

juce_add_plugin(SpringTank
    COMPANY_NAME "Dripworks"
    COMPANY_COPYRIGHT "Dripworks"
    BUNDLE_ID com.dripworks.springtank
    PLUGIN_MANUFACTURER_CODE Dwks
    PLUGIN_CODE SpTk
    PLUGIN_NAME "Spring Tank"
    PRODUCT_NAME "Spring Tank"
    DESCRIPTION "Vintage surf-amp spring tank reverb"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    FORMATS ${SPRINGTANK_FORMATS}
    AU_MAIN_TYPE kAudioUnitType_Effect
    VST3_CATEGORIES Fx Reverb
    AAX_CATEGORY AAX_ePlugInCategory_Reverb
    AAX_IDENTIFIER com.dripworks.springtank
)

juce_generate_juce_header(SpringTank)

add_library(SpringTankDSP STATIC
    Source/DSP/SpringReverb.cpp
)
target_include_directories(SpringTankDSP PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/Source
)
target_compile_definitions(SpringTankDSP
    PRIVATE
        JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
)
target_link_libraries(SpringTankDSP
    PUBLIC
        juce::juce_audio_basics
    PRIVATE
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

target_sources(SpringTank
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
)

target_compile_definitions(SpringTank
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_REPORT_APP_USAGE=0
)

target_link_libraries(SpringTank
    PRIVATE
        SpringTankDSP
        juce::juce_audio_utils
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# Offline impulse-response tool. No audio device and no GUI.
add_executable(SpringTankIRTest
    tests/ImpulseResponseTest.cpp
)
target_compile_definitions(SpringTankIRTest
    PRIVATE
        JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
)
target_link_libraries(SpringTankIRTest
    PRIVATE
        SpringTankDSP
        juce::juce_audio_formats
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
set_target_properties(SpringTankIRTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

enable_testing()
add_test(
    NAME spring_tank_impulse
    COMMAND $<TARGET_FILE:SpringTankIRTest> "${CMAKE_BINARY_DIR}/spring_tank_impulse.wav"
)
