cmake_minimum_required(VERSION 3.22)

project(InfinityDrum VERSION 1.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
    # Builds for the host architecture by default. Pass
    # -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" for a universal binary.
endif()

add_subdirectory(JUCE)

#===============================================================================
# Shared core: synthesis engines, track/processor, editor.
# Kept as its own library so the offline test and render tools can reuse it.
#===============================================================================
add_library(InfinityDrumCore STATIC
    Source/Core/Parameters.cpp
    Source/Core/ModulationState.cpp
    Source/Core/Sequencer.cpp
    Source/Core/Randomizer.cpp
    Source/Core/KitLibrary.cpp
    Source/Core/Settings.cpp
    Source/Core/DrumKitProcessor.cpp
    Source/UI/Theme.cpp
    Source/UI/ModelKnob.cpp
    Source/UI/VelocityKnob.cpp
    Source/UI/TrackStrip.cpp
    Source/UI/DrumKitEditor.cpp
    Source/UI/TrackHeader.cpp
    Source/UI/SequencerLane.cpp)

target_include_directories(InfinityDrumCore PUBLIC Source)

target_compile_definitions(InfinityDrumCore PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_MODAL_LOOPS_PERMITTED=0)

target_link_libraries(InfinityDrumCore PUBLIC
    juce::juce_audio_processors
    juce::juce_gui_basics
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

#===============================================================================
# The plugin itself.
#===============================================================================
juce_add_plugin(InfinityDrum
    COMPANY_NAME "InfinityDrum Audio"
    BUNDLE_ID "com.infinitydrumaudio.infinitydrum"
    PLUGIN_MANUFACTURER_CODE Infa
    PLUGIN_CODE In01
    # AU is Apple-only; JUCE skips it on other platforms, so the Windows build simply
    # produces the VST3.
    FORMATS VST3 AU
    PRODUCT_NAME "InfinityDrum"
    DESCRIPTION "Eight-track drum and percussion synthesiser"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    VST3_CATEGORIES "Instrument" "Drum"
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(InfinityDrum PRIVATE Source/PluginEntry.cpp)

target_link_libraries(InfinityDrum PRIVATE InfinityDrumCore)

target_compile_definitions(InfinityDrum PUBLIC
    JUCE_VST3_CAN_REPLACE_VST2=0)

# CMake signs the bundle as part of linking, but JUCE writes
# Contents/Resources/moduleinfo.json in a POST_BUILD step that runs *after* the link. A fresh
# build therefore ships a signature that does not seal that file, and macOS reports
# "a sealed resource is missing or invalid" - an invalid signature, not merely an ad-hoc one.
# Gatekeeper may then refuse the plugin even once the quarantine flag is cleared.
#
# Signing again here fixes it. This is registered after juce_add_plugin, and custom commands
# on a target run in the order they were added, so it runs after JUCE's own POST_BUILD steps
# and seals whatever is actually in the bundle by then. The verify command is deliberate: a
# regression should fail the build, not ship quietly.
if(APPLE)
    # Both bundles have to be signed again once their post-build steps have run. The VST3
    # because JUCE writes moduleinfo.json after the link, and the AU because its Info.plist
    # appears after CMake signs it - either way the signature ends up describing a bundle
    # that no longer matches, which macOS reports as an invalid signature.
    #
    # Registering these after juce_add_plugin puts them after JUCE's own post-build
    # commands, and the verify makes a regression fail the build rather than ship.
    foreach(bundle_target InfinityDrum_VST3 InfinityDrum_AU)
        add_custom_command(TARGET ${bundle_target} POST_BUILD
            COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:${bundle_target}>"
            COMMAND codesign --verify --verbose=2 "$<TARGET_BUNDLE_DIR:${bundle_target}>"
            COMMENT "Re-signing ${bundle_target} after its post-build steps"
            VERBATIM)
    endforeach()
endif()

#===============================================================================
# Offline tools (headless; they drive the processor directly).
#===============================================================================
juce_add_console_app(InfinityDrumRender PRODUCT_NAME "InfinityDrumRender")
target_sources(InfinityDrumRender PRIVATE Tools/RenderDemo.cpp)
target_link_libraries(InfinityDrumRender PRIVATE InfinityDrumCore juce::juce_audio_formats)

# Kit design harness: renders .dfkit files through lane-matched patterns.
juce_add_console_app(InfinityDrumKitLab PRODUCT_NAME "InfinityDrumKitLab")
target_sources(InfinityDrumKitLab PRIVATE Tools/KitLab.cpp)
target_link_libraries(InfinityDrumKitLab PRIVATE InfinityDrumCore juce::juce_audio_formats)

juce_add_console_app(InfinityDrumTests PRODUCT_NAME "InfinityDrumTests")
target_sources(InfinityDrumTests PRIVATE Tools/RenderTests.cpp)
target_link_libraries(InfinityDrumTests PRIVATE InfinityDrumCore)

# GUI smoke test: constructs the real editor and renders it to a PNG.
#
# A console app, not a gui_app, even though it drives the GUI. On Windows a gui_app target
# links with /SUBSYSTEM:WINDOWS, which needs WinMain - and only a JUCEApplication built with
# START_JUCE_APPLICATION provides one. This tool has a plain main(), so a gui_app target
# fails to link there. It is a command-line tool that renders offscreen, so the console
# subsystem is also the honest description of what it is.
juce_add_console_app(InfinityDrumScreenshot PRODUCT_NAME "InfinityDrumScreenshot")
target_sources(InfinityDrumScreenshot PRIVATE Tools/RenderScreenshot.cpp)
target_link_libraries(InfinityDrumScreenshot PRIVATE InfinityDrumCore)

# End-to-end test: loads the built .vst3 through JUCE's VST3 hosting code, exactly as
# a DAW would. Deliberately does NOT link InfinityDrumCore - it exercises the shipped
# binary as a black box, so the host-side code must be compiled into this target.
# Must be a gui_app rather than a console_app: on macOS JUCE drives the message loop with
# [NSApp run], which only works from a real app bundle.
juce_add_gui_app(InfinityDrumHostTest PRODUCT_NAME "InfinityDrumHostTest")
target_sources(InfinityDrumHostTest PRIVATE Tools/HostLoadTest.cpp)
target_compile_definitions(InfinityDrumHostTest PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_MODAL_LOOPS_PERMITTED=0)
target_link_libraries(InfinityDrumHostTest PRIVATE
    juce::juce_audio_processors
    juce::juce_gui_basics
    juce::juce_recommended_config_flags)
add_dependencies(InfinityDrumHostTest InfinityDrum)
