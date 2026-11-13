#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "LayerConfig.h"
#include "PerfMonitor.h"
#include "SampleData.h"

#include <array>
#include <functional>
#include <memory>
#include <vector>

// Time-axis preview of what plays when a note is triggered, for ALL layers at once:
//   ruler (ms)  /  every layer's waveform in its own colour  /  one delay handle per layer
// x = 0 is the trigger moment; the width spans `scaleMs`. Dragging a layer's handle delays that
// layer (its waveform slides along the ruler). This panel stays put when the tab changes.
//
// Efficiency: an outline is rebuilt only when that layer's file, region, the scale or the size
// changes. Dragging a handle only repaints the waveform + handle strip (the outline is translated)
// and writes one atomic per step.
class TimelineView : public juce::Component
{
public:
    explicit TimelineView (perf::Monitor&);

    // Called while dragging (committed = false) and on release (committed = true).
    std::function<void (int layer, float delayMs, bool committed)> onDelayChanged;

    void setLayerStyle (int layer, const juce::String& name, juce::Colour colour);
    void setSample (int layer, std::shared_ptr<const SampleData> newSample);   // also re-reads region + delay
    void setActiveLayer (int layer);                                           // drawn on top
    void setScaleMs (int ms);

    // Height this panel needs for its rows (ruler + minimum waveform + one handle row per layer).
    static int getMinimumHeight() noexcept { return 130 + layercfg::kCount * (int) (kRowHeight + kRowGap); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    static constexpr float kThumbWidth = 18.0f, kRowHeight = 14.0f, kRowGap = 3.0f;

    struct LayerView
    {
        juce::String name;
        juce::Colour colour { 0xff4fc3f7 };
        std::shared_ptr<const SampleData> sample;
        juce::Path path;
        int regionStart = 0, regionEnd = 0;
        float delayMs = 0.0f;
    };

    void rebuildPath (LayerView&);
    void dragTo (float mouseX);
    int rowAt (juce::Point<float> p) const noexcept;
    float msToX (double ms) const noexcept { return waveArea.getX() + (float) (ms / (double) scaleMs) * waveArea.getWidth(); }
    float thumbX (const LayerView&) const noexcept;
    juce::Rectangle<int> dirtyArea() const noexcept;

    perf::Monitor& perfMon;
    std::array<LayerView, (size_t) layercfg::kCount> layers;
    std::array<juce::Rectangle<float>, (size_t) layercfg::kCount> rows;
    juce::Rectangle<float> rulerArea, waveArea;
    std::vector<float> columnTop, columnBottom;   // scratch, reused

    int scaleMs = 1000;
    int activeLayer = 0;
    int dragLayer = -1;
    float grabOffset = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimelineView)
};
