#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Layer.h"
#include "PerfMonitor.h"

#include <array>
#include <memory>

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

    // used by the editor (layer 0 is the default, so single-layer code keeps working)
    void triggerFromGui() noexcept;                                   // the "Play" button: triggers every layer
    SampleLibrary& getLibrary (int layer = 0) noexcept   { return layers[(size_t) layer]->library; }
    LayerParams& getLayer (int layer = 0) noexcept       { return layers[(size_t) layer]->params; }   // speed + level
    perf::Monitor& getPerf() noexcept                    { return perfMonitor; }
    int getEditorWidth() const noexcept     { return editorWidth.load(); }
    int getEditorHeight() const noexcept    { return editorHeight.load(); }
    void setEditorSize (int w, int h) noexcept { editorWidth.store (w); editorHeight.store (h); }
    int getScaleMs() const noexcept         { return scaleMs.load(); }       // timeline zoom, saved with the project
    void setScaleMs (int ms) noexcept       { scaleMs.store (ms); }
    int getActiveTab() const noexcept       { return activeTab.load(); }     // which layer's tab is showing
    void setActiveTab (int i) noexcept      { activeTab.store (i); }

private:
    struct LayerRestore
    {
        std::vector<SampleLibrary::FileState> files;
        int selected = 0;
        std::shared_ptr<juce::XmlElement> xml;        // the layer's <Speed>/<Level> parameters
    };

    void sampleLibraryChanged() override;   // keeps a copy of the state that is safe to read from any thread
    void restoreOnMessageThread (const std::vector<LayerRestore>&);

    // Declaration order matters: the monitor must outlive the layers.
    perf::Monitor perfMonitor;
    std::array<std::unique_ptr<Layer>, (size_t) layercfg::kCount> layers;

    std::atomic<int> editorWidth { 780 }, editorHeight { 740 }, scaleMs { 1000 }, activeTab { 0 };

    juce::CriticalSection stateLock;       // never taken on the audio thread
    std::array<std::vector<SampleLibrary::FileState>, (size_t) layercfg::kCount> stateFiles;
    std::array<int, (size_t) layercfg::kCount> stateSelected {};

    JUCE_DECLARE_WEAK_REFERENCEABLE (SamplerProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SamplerProcessor)
};
