#pragma once

#include "LayerPage.h"
#include "ModControl.h"
#include "PerfOverlay.h"
#include "PluginProcessor.h"
#include "TimelineView.h"

#include <array>
#include <memory>

// Dark theme for the combo box, its popup menu, the buttons, the knobs and the layer tabs. Owned
// by the editor (not the global default) so other plugins in the host are unaffected.
class SamplerLookAndFeel : public juce::LookAndFeel_V4
{
public:
    SamplerLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawTabButton (juce::TabBarButton&, juce::Graphics&, bool isMouseOver, bool isMouseDown) override;
    int getTabButtonBestWidth (juce::TabBarButton&, int tabDepth) override;
    void drawTabAreaBehindFrontButton (juce::TabbedButtonBar&, juce::Graphics&, int w, int h) override;
};

// The row of layer tabs (ONSET, BODY, ...).
class LayerTabs : public juce::TabbedButtonBar
{
public:
    LayerTabs() : juce::TabbedButtonBar (juce::TabbedButtonBar::TabsAtTop) {}

    std::function<void (int)> onChange;
    void currentTabChanged (int newIndex, const juce::String&) override { if (onChange) onChange (newIndex); }
};

class SamplerEditor : public juce::AudioProcessorEditor,
                      public juce::FileDragAndDropTarget
{
public:
    explicit SamplerEditor (SamplerProcessor&);
    ~SamplerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;     // Ctrl+Z undo, Ctrl+Y / Ctrl+Shift+Z redo (active layer)

    // The whole window accepts dropped files; they go to the layer whose tab is showing.
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    void setActiveLayer (int layer);
    void layoutOverlay();
    LayerPage& activePage() noexcept { return *pages[(size_t) activeLayer]; }

    SamplerProcessor& processor;
    perf::Monitor& perfMon;

    SamplerLookAndFeel lookAndFeel;
    LayerTabs tabs;
    juce::TextButton playButton { "Play" };
    juce::TextButton perfButton { "Perf" };
    std::array<std::unique_ptr<LayerPage>, (size_t) layercfg::kCount> pages;
    TimelineView timeline;                       // shared by all layers: stays put when the tab changes
    juce::Slider scaleSlider;
    PerfOverlay overlay;

    int activeLayer = 0;
    juce::Rectangle<int> statusArea, scaleLabelArea, contentArea;

    juce::StringArray lastDragFiles;     // caches isInterestedInFileDrag (called on every mouse move)
    bool lastDragResult = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SamplerEditor)
};
