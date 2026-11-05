#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "PerfMonitor.h"
#include "SampleLibrary.h"

// Translucent panel showing the live numbers from perf::Monitor.
// It only refreshes (2 Hz) while visible, so it costs nothing when hidden.
class PerfOverlay : public juce::Component,
                    private juce::Timer
{
public:
    static constexpr int kPreferredWidth = 420;

    PerfOverlay (perf::Monitor&, SampleLibrary&);
    ~PerfOverlay() override { stopTimer(); }

    void setActive (bool shouldBeActive);
    int getPreferredHeight() const noexcept;

    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;

    perf::Monitor& monitor;
    SampleLibrary& library;
    juce::StringArray lines;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PerfOverlay)
};
