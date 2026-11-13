#pragma once

#include "ModControl.h"
#include "PluginProcessor.h"
#include "SelectionOverlay.h"
#include "WaveformView.h"

// Everything that belongs to ONE layer's tab: its file dropdown with undo / redo / clear, the
// waveform editor with region selection, and the SPEED / LEVEL controls. The editor creates one
// page per layer and shows the active one; the timeline and the Play button live outside.
class LayerPage : public juce::Component,
                  private SampleLibrary::Listener,
                  private juce::ChangeListener
{
public:
    LayerPage (SamplerProcessor&, int layerIndex);
    ~LayerPage() override;

    // Tell the editor what changed so it can update the shared timeline and the status line.
    std::function<void (int layer, std::shared_ptr<const SampleData>)> onSelectedSampleChanged;
    std::function<void()> onStatusChanged;

    SampleLibrary& getLibrary() noexcept        { return library; }
    const juce::String& getStatusText() const noexcept { return statusText; }
    bool isStatusWarning() const noexcept       { return statusIsWarning; }
    void setDropHighlight (bool shouldHighlight) { waveform.setDropHighlight (shouldHighlight); }

    void resized() override;

private:
    void sampleLibraryChanged() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;   // project loaded: refresh the controls
    void refreshFromLibrary();

    SamplerProcessor& processor;
    SampleLibrary& library;
    perf::Monitor& perfMon;
    const int index;

    juce::ComboBox fileBox;
    juce::TextButton undoButton { "Undo" }, redoButton { "Redo" }, clearButton { "Clear" };
    WaveformView waveform;
    SelectionOverlay selection;
    ModControl speedControl, levelControl;

    juce::StringArray menuNames;         // what the dropdown currently shows (rebuilt only when it changes)
    juce::String statusText;
    bool statusIsWarning = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LayerPage)
};
