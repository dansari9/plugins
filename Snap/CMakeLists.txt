cmake_minimum_required(VERSION 3.22)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal macOS binary")
endif()

# Bump on every build that is handed out: Logic caches Audio Units by version.
project(Snap VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# No fused multiply-add contraction, so float results (parameter round trips, saved values, renders) are the same on
# every platform: GCC on x86-64-v3 and clang on arm64 contract by default (-24 + 48 * x gives -3.4999995 for -3.5 dB).
if(NOT MSVC)
    add_compile_options(-ffp-contract=off)
endif()

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
# COMPANY_NAME and the lowercase PLUGIN_NAME / PRODUCT_NAME are display-only; the codes above may not change.
juce_add_plugin(Snap
    COMPANY_NAME "daveydidit"
    COMPANY_COPYRIGHT "daveydidit"
    BUNDLE_ID com.dripworks.snap
    PLUGIN_MANUFACTURER_CODE Dwks
    PLUGIN_CODE Snap
    PLUGIN_NAME "snap"
    PRODUCT_NAME "snap"
    DESCRIPTION "Drum multi-FX"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    FORMATS ${SNAP_FORMATS}
    AU_MAIN_TYPE kAudioUnitType_Effect
    VST3_CATEGORIES Fx Dynamics Distortion Reverb
)

target_sources(Snap PRIVATE
    Source/PluginProcessor.cpp
    Source/ui/Editor.cpp
    Source/ui/Widgets.cpp
    Source/ui/Style.cpp
    Source/ui/Visualizers.cpp
    Source/ui/PresetBrowser.cpp
    Source/Parameters.cpp
    Source/presets/StateIO.cpp
    Source/presets/PresetStore.cpp
    Source/dsp/modules/Space.cpp
    Source/dsp/modules/SubHiss.cpp
)
juce_add_binary_data(SnapFonts SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/Assets/Fonts/GeistMono-Regular.ttf
    ${CMAKE_CURRENT_SOURCE_DIR}/Assets/Fonts/GeistMono-SemiBold.ttf)
# The generated impulse responses (space) and noise loops (hiss), embedded; regenerate with Tools/generate_*.py.
file(GLOB snap_irs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Resources/irs/*.wav)
file(GLOB snap_noise CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Resources/noise/*.wav)
juce_add_binary_data(SnapSpaceIRs HEADER_NAME SpaceIRData.h NAMESPACE SpaceIRData SOURCES ${snap_irs})
juce_add_binary_data(SnapSubHissNoise HEADER_NAME SubHissNoiseData.h NAMESPACE SubHissNoiseData SOURCES ${snap_noise})
# The factory presets (read-only; user presets are files in the user folder, see Source/presets/PresetStore.h).
file(GLOB snap_presets CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Resources/presets/*.snappreset)
juce_add_binary_data(SnapFactoryPresets HEADER_NAME FactoryPresetData.h NAMESPACE FactoryPresetData SOURCES ${snap_presets})

target_compile_definitions(Snap
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_FORCE_USE_LEGACY_PARAM_IDS=0
        JUCE_REPORT_APP_USAGE=0
)

target_link_libraries(Snap
    PRIVATE
        SnapFonts
        SnapSpaceIRs
        SnapSubHissNoise
        SnapFactoryPresets
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# One test executable that drives the real processor and editor without a host.
# `SnapTests --screenshot <file.png>` renders the editor instead of running the tests.
add_executable(SnapTests Tests/SnapTests.cpp Tests/DspTests.cpp Tests/PressTests.cpp Tests/DriveTests.cpp Tests/PushTests.cpp
    Tests/ShiftTests.cpp Tests/MasterTests.cpp Tests/SpaceTests.cpp Tests/SubHissTests.cpp Tests/ChainTests.cpp Tests/PresetTests.cpp)
target_include_directories(SnapTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
target_link_libraries(SnapTests
    PRIVATE
        Snap
        SnapSpaceIRs
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
# The DSP headers and tests compare floats bit-exactly on purpose (exact bypass paths); the tests also replace the global
# operator new.
if(NOT MSVC)
    set_source_files_properties(Source/PluginProcessor.cpp Source/ui/Editor.cpp Source/ui/Widgets.cpp Source/ui/Visualizers.cpp
                                Source/ui/PresetBrowser.cpp Source/dsp/modules/Space.cpp
                                Source/dsp/modules/SubHiss.cpp Source/presets/PresetStore.cpp PROPERTIES COMPILE_OPTIONS "-Wno-float-equal")
    set_source_files_properties(Tests/SnapTests.cpp PROPERTIES COMPILE_OPTIONS "-Wno-float-equal;-Wno-mismatched-new-delete")
    set_source_files_properties(Tests/DspTests.cpp Tests/PressTests.cpp Tests/DriveTests.cpp Tests/PushTests.cpp Tests/ShiftTests.cpp Tests/MasterTests.cpp
                                Tests/SpaceTests.cpp Tests/SubHissTests.cpp Tests/ChainTests.cpp Tests/PresetTests.cpp PROPERTIES COMPILE_OPTIONS "-Wno-float-equal")
endif()
set_target_properties(SnapTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
