#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "PerfMonitor.h"
#include "SampleData.h"
#include "WaveformView.h"

#include <functional>
#include <memory>

// Transparent layer on top of the waveform: shows which region plays and lets
// the user change it.
//
//   drag on the waveform      new region
//   drag inside the region    move it
//   drag a region edge        adjust that edge
//   double-click              whole file again
//   mouse wheel               zoom (shift + wheel or sideways wheel: pan)
//   right-click               zoom to selection / fit, select all
//
// Only this layer repaints while dragging (a few rectangles); the waveform
// underneath is a cached image. The region is sent to the library only when
// the mouse is released.
class SelectionOverlay : public juce::Component
{
public:
    SelectionOverlay (perf::Monitor&, WaveformView&);

    std::function<void (int start, int end)> onCommit;

    void setSample (std::shared_ptr<const SampleData> newSample);   // also re-reads the region
    void setAccent (juce::Colour c)       { accent = c; repaint(); }

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    enum class Drag { None, NewRegion, Start, End, Move };

    int numFrames() const noexcept { return sample != nullptr ? sample->numFrames : 0; }
    float xOf (int frame) const noexcept;
    int frameAt (float x) const noexcept;      // follows the zoom; not limited to the visible part
    void showMenu();
    void readRegionFromSample();

    perf::Monitor& perfMon;
    WaveformView& waveform;
    std::shared_ptr<const SampleData> sample;

    int regionStart = 0, regionEnd = 0;   // what is drawn (live while dragging)
    Drag drag = Drag::None;
    juce::Colour accent { 0xff4fc3f7 };
    int anchorFrame = 0;                  // frame where a new region or a move began
    int savedStart = 0, savedEnd = 0;     // region before the drag began

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SelectionOverlay)
};
