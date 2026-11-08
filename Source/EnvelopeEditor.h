#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Envelope.h"
#include "PerfMonitor.h"

#include <functional>

// A few draggable breakpoints. Click empty space: add a point (up to 8). Drag: move it.
// Double-click a point: remove it. The first / last point only move up and down.
// onChanged fires on every edit step (the audio side update is a ~1 KB table, a few microseconds).
class EnvelopeEditor : public juce::Component
{
public:
    explicit EnvelopeEditor (perf::Monitor&);

    std::function<void (const mod::Envelope&)> onChanged;

    void setEnvelope (const mod::Envelope&);          // from the model; does not call onChanged

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    juce::Point<float> toPixel (mod::Point p) const noexcept;
    mod::Point fromPixel (juce::Point<float> px) const noexcept;
    int hitTest (juce::Point<float> px) const noexcept;
    void edited();

    perf::Monitor& perfMon;
    mod::Envelope env = mod::Envelope::flat (0.5f);
    juce::Rectangle<float> area;
    int dragIndex = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EnvelopeEditor)
};
