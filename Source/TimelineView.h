#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "PerfMonitor.h"
#include "SampleData.h"

#include <functional>
#include <memory>
#include <vector>

// Time-axis preview of what plays when a note is triggered:
//   ruler (ms)  /  waveform of the selected region  /  delay handle
// x = 0 is the trigger moment; the width spans `scaleMs`. Dragging the handle
// under the waveform delays the sound (the waveform slides along the ruler).
//
// Efficiency: the outline is rebuilt only when the file, region, scale or size
// changes. Dragging the handle only repaints the waveform + handle strip
// (the outline is just translated), and writes one atomic per step.
class TimelineView : public juce::Component
{
public:
    explicit TimelineView (perf::Monitor&);

    // Called while dragging (committed = false) and on release (committed = true).
    std::function<void (float delayMs, bool committed)> onDelayChanged;

    void setSample (std::shared_ptr<const SampleData> newSample);   // also re-reads region + delay
    void setScaleMs (int ms);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    static constexpr float kThumbWidth = 18.0f;

    void rebuildPath();
    void dragTo (float mouseX);
    float msToX (double ms) const noexcept { return waveArea.getX() + (float) (ms / (double) scaleMs) * waveArea.getWidth(); }
    float thumbX() const noexcept;

    perf::Monitor& perfMon;
    std::shared_ptr<const SampleData> sample;
    juce::Path path;
    juce::Rectangle<float> rulerArea, waveArea, handleArea;
    std::vector<float> columnTop, columnBottom;   // scratch, reused

    int scaleMs = 1000;
    int regionStart = 0, regionEnd = 0;
    float delayMs = 0.0f;
    bool dragging = false;
    float grabOffset = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimelineView)
};
