cmake_minimum_required(VERSION 3.22)

# Statically link the MSVC runtime so the plugin runs without installing
# the Visual C++ Redistributable. Must be set before project().
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(HelloPlugin VERSION 0.3.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compile in the performance monitor (counters + the "Perf" overlay button).
# Turn OFF for a final release build: every measurement then compiles to nothing.
option(SAMPLER_PERF_MONITOR   "Enable the performance monitor" ON)
# Developer targets (not built by default):
option(SAMPLER_BUILD_TESTS      "Build the automated tests (SamplerTests)" OFF)
option(SAMPLER_BUILD_BENCHMARKS "Build the benchmark program (SamplerBenchmarks)" OFF)
option(SAMPLER_SANITIZE         "Address/UB sanitizers on this project's own sources (GCC/Clang)" OFF)

# Download JUCE automatically (pinned to a specific release)
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.6
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

# Audio engine, loader, profiler: no GUI code (also used by the benchmarks).
set(SAMPLER_CORE_SOURCES
    Source/PerfMonitor.cpp
    Source/SampleLibrary.cpp
    Source/SamplerEngine.cpp
    Source/WaveformGeometry.cpp
)
# Plugin wrapper and user interface.
set(SAMPLER_UI_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/WaveformView.cpp
    Source/SelectionOverlay.cpp
    Source/TimelineView.cpp
    Source/FileMenuItem.cpp
    Source/PerfOverlay.cpp
)
set(SAMPLER_DEFINITIONS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_MP3AUDIOFORMAT=1
    SAMPLER_PERF_ENABLED=$<BOOL:${SAMPLER_PERF_MONITOR}>
)

# ---------------------------------------------------------------- the plugin
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

target_sources(HelloPlugin PRIVATE ${SAMPLER_CORE_SOURCES} ${SAMPLER_UI_SOURCES})

target_compile_definitions(HelloPlugin PUBLIC
    ${SAMPLER_DEFINITIONS}
    JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(HelloPlugin
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# ----------------------------------------------------------- developer targets
if(SAMPLER_SANITIZE AND NOT MSVC)
    set(SAMPLER_SAN_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
    set_source_files_properties(${SAMPLER_CORE_SOURCES} ${SAMPLER_UI_SOURCES} Tests/Tests.cpp Benchmarks/Benchmarks.cpp
                                PROPERTIES COMPILE_OPTIONS "${SAMPLER_SAN_FLAGS}")
endif()

if(SAMPLER_BUILD_TESTS)
    juce_add_console_app(SamplerTests PRODUCT_NAME "SamplerTests")
    target_sources(SamplerTests PRIVATE Tests/Tests.cpp ${SAMPLER_CORE_SOURCES} ${SAMPLER_UI_SOURCES})
    target_include_directories(SamplerTests PRIVATE Source)
    target_compile_definitions(SamplerTests PRIVATE ${SAMPLER_DEFINITIONS} JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(SamplerTests PRIVATE juce::juce_audio_utils juce::juce_recommended_config_flags)
    if(SAMPLER_SANITIZE AND NOT MSVC)
        target_link_options(SamplerTests PRIVATE ${SAMPLER_SAN_FLAGS})
    endif()
endif()

if(SAMPLER_BUILD_BENCHMARKS)
    juce_add_console_app(SamplerBenchmarks PRODUCT_NAME "SamplerBenchmarks")
    target_sources(SamplerBenchmarks PRIVATE Benchmarks/Benchmarks.cpp ${SAMPLER_CORE_SOURCES})
    target_include_directories(SamplerBenchmarks PRIVATE Source)
    target_compile_definitions(SamplerBenchmarks PRIVATE ${SAMPLER_DEFINITIONS} JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(SamplerBenchmarks
        PRIVATE
            juce::juce_audio_formats
            juce::juce_graphics
            juce::juce_events
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags)
    if(SAMPLER_SANITIZE AND NOT MSVC)
        target_link_options(SamplerBenchmarks PRIVATE ${SAMPLER_SAN_FLAGS})
    endif()
endif()
