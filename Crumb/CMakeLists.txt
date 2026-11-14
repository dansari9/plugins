cmake_minimum_required(VERSION 3.22)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal macOS binary")
endif()

# Bump on every build that is handed out: Logic caches Audio Units by version.
project(Crumb VERSION 0.2.0 LANGUAGES C CXX)

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

set(CRUMB_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND CRUMB_FORMATS AU)
endif()

# FROZEN IDENTIFIERS - never change them, or hosts lose the plug-in and saved sessions:
# AU aufx / PLUGIN_CODE Crmb / PLUGIN_MANUFACTURER_CODE Dwks / BUNDLE_ID com.dripworks.crumb.
# PLUGIN_NAME / PRODUCT_NAME ("crumb", also the bundle file names) and COMPANY_NAME (Logic menu group, VST3 vendor) are
# display-only and may change before release; the codes above may not.
juce_add_plugin(Crumb
    COMPANY_NAME "daveydidit"
    COMPANY_COPYRIGHT "daveydidit"
    BUNDLE_ID com.dripworks.crumb
    PLUGIN_MANUFACTURER_CODE Dwks
    PLUGIN_CODE Crmb
    PLUGIN_NAME "crumb"
    PRODUCT_NAME "crumb"
    DESCRIPTION "Vintage-sampler bit crusher"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    FORMATS ${CRUMB_FORMATS}
    AU_MAIN_TYPE kAudioUnitType_Effect
    VST3_CATEGORIES Fx Distortion
)

target_sources(Crumb PRIVATE Source/Crumb.cpp Source/CrumbEditor.cpp)

# Geist Mono (SIL OFL 1.1, Assets/Fonts/OFL.txt), the same faces as Rootnote.
juce_add_binary_data(CrumbFonts SOURCES
    Assets/Fonts/GeistMono-Regular.ttf Assets/Fonts/GeistMono-Medium.ttf Assets/Fonts/GeistMono-SemiBold.ttf)
set_target_properties(CrumbFonts PROPERTIES POSITION_INDEPENDENT_CODE TRUE)

target_compile_definitions(Crumb
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_FORCE_USE_LEGACY_PARAM_IDS=0
        JUCE_REPORT_APP_USAGE=0
)

target_link_libraries(Crumb
    PRIVATE
        CrumbFonts
        juce::juce_audio_utils
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# One test executable that drives the real processor and editor without a host.
add_executable(CrumbTests Tests/CrumbTests.cpp)
target_include_directories(CrumbTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
target_link_libraries(CrumbTests
    PRIVATE
        CrumbFonts
        juce::juce_cryptography
        Crumb
        juce::juce_audio_utils
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
# The tests compare floats bit-exactly on purpose and replace the global operator new.
if(NOT MSVC)
    set_source_files_properties(Tests/CrumbTests.cpp PROPERTIES COMPILE_OPTIONS "-Wno-float-equal;-Wno-mismatched-new-delete")
endif()
set_target_properties(CrumbTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_compile_definitions(CrumbTests PRIVATE JUCE_MODAL_LOOPS_PERMITTED=1) # public message pump for the editor tests
