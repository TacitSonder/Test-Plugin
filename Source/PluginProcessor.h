#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "LayerParams.h"
#include "PerfMonitor.h"
#include "SampleLibrary.h"
#include "SamplerEngine.h"

class SamplerProcessor : public juce::AudioProcessor,
                         private SampleLibrary::Listener
{
public:
    SamplerProcessor();
    ~SamplerProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    using AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Sample Drop"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // used by the editor
    void triggerFromGui() noexcept          { engine.requestTrigger(); }   // the "Play" button
    SampleLibrary& getLibrary() noexcept    { return library; }
    LayerParams& getLayer() noexcept        { return layerParams; }          // speed + level of the (single, for now) layer
    perf::Monitor& getPerf() noexcept       { return perfMonitor; }
    int getEditorWidth() const noexcept     { return editorWidth.load(); }
    int getEditorHeight() const noexcept    { return editorHeight.load(); }
    void setEditorSize (int w, int h) noexcept { editorWidth.store (w); editorHeight.store (h); }
    int getScaleMs() const noexcept         { return scaleMs.load(); }       // timeline zoom, saved with the project
    void setScaleMs (int ms) noexcept       { scaleMs.store (ms); }

private:
    void sampleLibraryChanged() override;
    void restoreOnMessageThread (const std::vector<SampleLibrary::FileState>&, int selected, const juce::XmlElement* layerXml);   // keeps a copy of the state that is safe to read from any thread

    // Declaration order matters: the monitor and library must outlive the engine.
    perf::Monitor perfMonitor;
    SampleLibrary library;
    LayerParams layerParams;
    SamplerEngine engine;

    std::atomic<int> editorWidth { 760 }, editorHeight { 640 }, scaleMs { 1000 };

    juce::CriticalSection stateLock;       // never taken on the audio thread
    std::vector<SampleLibrary::FileState> stateFiles;
    int stateSelected = 0;

    JUCE_DECLARE_WEAK_REFERENCEABLE (SamplerProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SamplerProcessor)
};
