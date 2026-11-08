#pragma once

#include "PerfOverlay.h"
#include "PluginProcessor.h"
#include "SelectionOverlay.h"
#include "TimelineView.h"
#include "WaveformView.h"

// Dark theme for the combo box, its popup menu and the buttons. Owned by the
// editor (not the global default) so other plugins in the host are unaffected.
class SamplerLookAndFeel : public juce::LookAndFeel_V4
{
public:
    SamplerLookAndFeel();
};

class SamplerEditor : public juce::AudioProcessorEditor,
                      public juce::FileDragAndDropTarget,
                      private SampleLibrary::Listener
{
public:
    explicit SamplerEditor (SamplerProcessor&);
    ~SamplerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;     // Ctrl+Z undo, Ctrl+Y / Ctrl+Shift+Z redo

    // The whole window accepts dropped files.
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    void sampleLibraryChanged() override;
    void refreshFromLibrary();
    void layoutOverlay();

    SamplerProcessor& processor;
    SampleLibrary& library;
    perf::Monitor& perfMon;

    SamplerLookAndFeel lookAndFeel;
    juce::ComboBox fileBox;
    juce::TextButton undoButton { "Undo" };
    juce::TextButton redoButton { "Redo" };
    juce::TextButton playButton { "Play" };
    juce::TextButton clearButton { "Clear" };
    juce::TextButton perfButton { "Perf" };
    WaveformView waveform;
    SelectionOverlay selection;
    TimelineView timeline;
    juce::Slider scaleSlider;
    juce::Rectangle<int> scaleLabelArea, contentArea;
    PerfOverlay overlay;

    juce::StringArray menuNames;         // what the dropdown currently shows (rebuilt only when it changes)

    juce::String statusText;
    bool statusIsWarning = false;
    juce::Rectangle<int> statusArea;

    juce::StringArray lastDragFiles;     // caches isInterestedInFileDrag (called on every mouse move)
    bool lastDragResult = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SamplerEditor)
};
