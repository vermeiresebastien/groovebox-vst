cmake_minimum_required(VERSION 3.22)

project(GrooveboxVST VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (MSVC)
    # Static CRT keeps the VST3 self-contained (no VC++ redistributable needed).
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    # /MP is deliberately not enabled. Combined with MSBuild's own parallel
    # projects it oversubscribes the machine, and MSVC 14.51 then fails with
    # intermittent "internal compiler error" / D8040 on JUCE's larger modules.
    # Build with `--parallel 2` to keep the number of cl.exe processes bounded.
    add_compile_options(/Zc:__cplusplus)
endif()

# ---------------------------------------------------------------------------
# JUCE
#
# JUCE is cloned explicitly rather than through FetchContent_Declare: the
# FetchContent helper builds a nested sub-project whose own git invocation does
# not pick up this repository's local git configuration, and on this machine
# git's default schannel backend cannot acquire credentials. Clone it here, then
# hand the populated directory to FetchContent.
# ---------------------------------------------------------------------------
set(JUCE_VERSION "8.0.4")
set(JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/JUCE")

if (NOT EXISTS "${JUCE_DIR}/CMakeLists.txt")
    message(STATUS "Cloning JUCE ${JUCE_VERSION} into external/JUCE ...")

    find_package(Git REQUIRED)

    file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/external")

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -c http.sslBackend=openssl
                clone --quiet --depth 1 --branch "${JUCE_VERSION}"
                https://github.com/juce-framework/JUCE.git "${JUCE_DIR}"
        RESULT_VARIABLE juce_clone_result
        OUTPUT_VARIABLE juce_clone_output
        ERROR_VARIABLE  juce_clone_output
    )

    if (NOT juce_clone_result EQUAL 0)
        message(FATAL_ERROR
            "Could not clone JUCE ${JUCE_VERSION}.\n${juce_clone_output}\n"
            "Clone it manually into ${JUCE_DIR} and re-run CMake.")
    endif()
endif()

include(FetchContent)

set(JUCE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JUCE_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(JUCE SOURCE_DIR "${JUCE_DIR}")
FetchContent_MakeAvailable(JUCE)

# ---------------------------------------------------------------------------
# Shared plugin code
#
# The processor, the DSP and the editor are compiled into both the plugin and
# the headless smoke test. They are compiled twice rather than moved into a
# static library because JUCE only generates its JuceHeader.h for targets made
# with a juce_add_* function, and the plugin and the test need different
# JUCE_STANDALONE_APPLICATION settings anyway.
# ---------------------------------------------------------------------------
set(GROOVEBOX_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/SampleStore.cpp
    Source/SequencerEngine.cpp
    Source/StepGridComponent.cpp
    Source/ChannelStrip.cpp
    Source/TransportBar.cpp
    Source/LookAndFeel.cpp
)

set(GROOVEBOX_DEFINES
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
)

# ---------------------------------------------------------------------------
# Plugin target
# ---------------------------------------------------------------------------
juce_add_plugin(Groovebox
    COMPANY_NAME              "Earworm"
    PLUGIN_MANUFACTURER_CODE   Eruw
    PLUGIN_CODE                Grvb
    PRODUCT_NAME              "Groovebox"
    DESCRIPTION               "Drag-and-drop sample step sequencer with per-channel pitch and swing"
    FORMATS                   VST3 Standalone
    IS_SYNTH                  TRUE
    # A VST3 instrument must expose an event input bus. With NEEDS_MIDI_INPUT
    # off there is no such bus, and hosts reject the plugin outright — Ableton
    # Live reports "plugin has instrument category, but no valid event input
    # bus". The sequencer does not read MIDI; the bus exists so the plugin is a
    # well-formed instrument.
    NEEDS_MIDI_INPUT          TRUE
    NEEDS_MIDI_OUTPUT         FALSE
    IS_MIDI_EFFECT            FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD   FALSE
    VST3_CATEGORIES           "Instrument;Drum"
)

target_sources(Groovebox PRIVATE ${GROOVEBOX_SOURCES})

target_include_directories(Groovebox PRIVATE Source)

target_compile_definitions(Groovebox PUBLIC ${GROOVEBOX_DEFINES})

# The sources use the classic `#include <JuceHeader.h>` style, which JUCE only
# emits when this is called.
juce_generate_juce_header(Groovebox)

target_link_libraries(Groovebox PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

# ---------------------------------------------------------------------------
# Headless smoke test
#
# Drives the real processor with no UI and no host, and checks the timing,
# swing, pitch and state round-trip behaviour.
# ---------------------------------------------------------------------------
juce_add_console_app(GrooveboxSmokeTest
    PRODUCT_NAME "GrooveboxSmokeTest"
)

target_sources(GrooveboxSmokeTest PRIVATE
    ${GROOVEBOX_SOURCES}
    Tests/SmokeTest.cpp
)

target_include_directories(GrooveboxSmokeTest PRIVATE Source)

target_compile_definitions(GrooveboxSmokeTest PUBLIC ${GROOVEBOX_DEFINES})

juce_generate_juce_header(GrooveboxSmokeTest)

target_link_libraries(GrooveboxSmokeTest PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

# ---------------------------------------------------------------------------
# Editor layout check
#
# Builds the real editor and reports the bounds of every control, so a control
# laid out off-panel or at zero height is caught without needing to look at a GUI.
# ---------------------------------------------------------------------------
juce_add_console_app(GrooveboxLayoutTest
    PRODUCT_NAME "GrooveboxLayoutTest"
)

target_sources(GrooveboxLayoutTest PRIVATE
    ${GROOVEBOX_SOURCES}
    Tests/LayoutTest.cpp
)

target_include_directories(GrooveboxLayoutTest PRIVATE Source)

target_compile_definitions(GrooveboxLayoutTest PUBLIC ${GROOVEBOX_DEFINES})

juce_generate_juce_header(GrooveboxLayoutTest)

target_link_libraries(GrooveboxLayoutTest PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)
