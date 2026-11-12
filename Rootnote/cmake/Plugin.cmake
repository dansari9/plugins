# The JUCE plug-in (VST3, Standalone, AU on Apple) and its test executable.
# Included from the top-level CMakeLists.txt after RootnoteCore is defined.

include(FetchContent)
FetchContent_Declare(
    JUCE
    URL https://github.com/juce-framework/JUCE/archive/refs/tags/8.0.15.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(JUCE)

set(ROOTNOTE_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND ROOTNOTE_FORMATS AU)
endif()

if(ROOTNOTE_COPY_AFTER_BUILD)
    set_property(GLOBAL PROPERTY JUCE_COPY_PLUGIN_AFTER_BUILD TRUE)
endif()

# The AU type (aumf) and the two codes identify the plug-in to hosts and to
# saved sessions. Never change them after a release.
juce_add_plugin(Rootnote
    COMPANY_NAME "Dripworks"
    COMPANY_COPYRIGHT "Dripworks"
    BUNDLE_ID com.dripworks.rootnote
    PLUGIN_MANUFACTURER_CODE Dwks
    PLUGIN_CODE RtNt
    PLUGIN_NAME "Rootnote"
    PRODUCT_NAME "Rootnote"
    DESCRIPTION "Note-based resonant drum-tuning EQ"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    FORMATS ${ROOTNOTE_FORMATS}
    AU_MAIN_TYPE kAudioUnitType_MusicEffect
    VST3_CATEGORIES Fx EQ Filter
)

target_sources(Rootnote
    PRIVATE
        Source/Parameters.cpp
        Source/Presets.cpp
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/ui/Pages.cpp
        Source/ui/Screen.cpp
        Source/ui/Theme.cpp
        Source/ui/Widgets.cpp
)

# The editor's typeface: Geist Mono (OFL), built by Assets/Fonts/make_fonts.py.
juce_add_binary_data(RootnoteFonts
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/Assets/Fonts/GeistMono-Regular.ttf
        ${CMAKE_CURRENT_SOURCE_DIR}/Assets/Fonts/GeistMono-Medium.ttf
        ${CMAKE_CURRENT_SOURCE_DIR}/Assets/Fonts/GeistMono-SemiBold.ttf
)

target_compile_definitions(Rootnote
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_REPORT_APP_USAGE=0
)

target_link_libraries(Rootnote
    PRIVATE
        RootnoteCore
        RootnoteFonts
        juce::juce_audio_utils
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

if(ROOTNOTE_BUILD_TESTS)
    # Drives the real processor and editor without a host or an audio device.
    add_executable(RootnotePluginTests Tests/PluginTests.cpp)
    target_include_directories(RootnotePluginTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Tests)
    target_link_libraries(RootnotePluginTests
        PRIVATE
            Rootnote
            RootnoteCore
            juce::juce_audio_utils
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags
    )
    set_target_properties(RootnotePluginTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME rootnote_plugin COMMAND RootnotePluginTests)
endif()

# The core compares settings for exact equality on purpose, to detect changes.
# Set per source file so it comes after JUCE's recommended warning flags.
if(NOT MSVC)
    set_source_files_properties(
        Source/Parameters.cpp Source/Presets.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp
        Source/ui/Pages.cpp Source/ui/Screen.cpp Source/ui/Theme.cpp Source/ui/Widgets.cpp Tests/PluginTests.cpp
        PROPERTIES COMPILE_OPTIONS -Wno-float-equal)
endif()
