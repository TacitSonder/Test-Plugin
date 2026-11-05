cmake_minimum_required(VERSION 3.22)

# Statically link the MSVC runtime so the plugin runs without installing
# the Visual C++ Redistributable. Must be set before project().
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(HelloPlugin VERSION 0.2.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compile in the performance monitor (counters + the "Perf" overlay button).
# Turn OFF for a final release build: every measurement then compiles to nothing.
option(SAMPLER_PERF_MONITOR "Enable the performance monitor" ON)

# Download JUCE automatically (pinned to a specific release)
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.6
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

# The CMake target keeps the name "HelloPlugin" so the GitHub workflow does not
# need to change; the plugin itself is called "Sample Drop".
juce_add_plugin(HelloPlugin
    COMPANY_NAME "MyCompany"
    PLUGIN_MANUFACTURER_CODE Mycp
    PLUGIN_CODE Sdrp
    FORMATS VST3
    PRODUCT_NAME "Sample Drop"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    VST3_CATEGORIES Instrument Sampler
    COPY_PLUGIN_AFTER_BUILD FALSE
)

target_sources(HelloPlugin PRIVATE
    Source/PerfMonitor.cpp
    Source/SampleLibrary.cpp
    Source/SamplerEngine.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/WaveformView.cpp
    Source/PerfOverlay.cpp
)

target_compile_definitions(HelloPlugin PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_VST3_CAN_REPLACE_VST2=0
    SAMPLER_PERF_ENABLED=$<BOOL:${SAMPLER_PERF_MONITOR}>
)

target_link_libraries(HelloPlugin
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
