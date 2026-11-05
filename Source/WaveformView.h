#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "PerfMonitor.h"
#include "SampleData.h"

#include <memory>
#include <vector>

// Draws the waveform of one sample from its precomputed peaks.
//
// Efficiency: the outline is rebuilt only when the sample or the size changes
// (O(width), not O(samples)), and the component is cached to an image, so
// repaints caused by other components (e.g. the perf overlay) cost a blit.
class WaveformView : public juce::Component
{
public:
    explicit WaveformView (perf::Monitor&);

    void setSample (std::shared_ptr<const SampleData> newSample);
    void setDropHighlight (bool shouldHighlight);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void rebuildPath();

    perf::Monitor& perfMon;
    std::shared_ptr<const SampleData> sample;
    juce::Path path;
    juce::Rectangle<float> waveArea;
    juce::String info;
    std::vector<float> columnTop, columnBottom;   // scratch, reused between rebuilds
    bool dropHighlight = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformView)
};
