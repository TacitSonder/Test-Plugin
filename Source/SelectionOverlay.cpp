#include "SelectionOverlay.h"
#include "UiTheme.h"

namespace
{
    constexpr float kEdgeTolerance = 7.0f;   // pixels
    constexpr float kClickSlop = 3.0f;       // movement below this is a click, not a drag
}

SelectionOverlay::SelectionOverlay (perf::Monitor& pm, const WaveformView& w) : perfMon (pm), waveform (w)
{
    setOpaque (false);
}

void SelectionOverlay::setSample (std::shared_ptr<const SampleData> newSample)
{
    sample = std::move (newSample);

    if (drag == Drag::None)
        readRegionFromSample();

    repaint();
}

void SelectionOverlay::readRegionFromSample()
{
    if (sample != nullptr)
        sample->getRegion (regionStart, regionEnd);
    else
        regionStart = regionEnd = 0;
}

float SelectionOverlay::xOf (int frame) const noexcept
{
    const auto a = waveform.getWaveArea();
    return region::frameToX (frame, numFrames(), a.getX(), a.getWidth());
}

int SelectionOverlay::frameAt (float x) const noexcept
{
    const auto a = waveform.getWaveArea();
    return region::xToFrame (x, numFrames(), a.getX(), a.getWidth());
}

// ---- drawing ------------------------------------------------------------------
void SelectionOverlay::paint (juce::Graphics& g)
{
    perf::ScopedTimer timer (perfMon, perf::Id::SelectionPaint);

    if (sample == nullptr || numFrames() <= 0)
        return;

    const auto area = waveform.getWaveArea();
    if (area.isEmpty())
        return;

    const float xs = xOf (regionStart), xe = xOf (regionEnd);
    const bool full = region::isFull (regionStart, regionEnd, numFrames());

    if (! full)
    {
        g.setColour (juce::Colours::black.withAlpha (0.55f));                     // dim what will not play
        g.fillRect (area.getX(), area.getY(), juce::jmax (0.0f, xs - area.getX()), area.getHeight());
        g.fillRect (xe, area.getY(), juce::jmax (0.0f, area.getRight() - xe), area.getHeight());

        g.setColour (theme::accent.withAlpha (0.10f));                            // tint what will play
        g.fillRect (xs, area.getY(), xe - xs, area.getHeight());
    }

    // edges with a small grip on top
    g.setColour (full ? theme::subtle.withAlpha (0.55f) : theme::accent);
    for (const float x : { xs, xe })
    {
        g.fillRect (x - 0.5f, area.getY(), 1.0f, area.getHeight());
        g.fillRect (x - 3.0f, area.getY(), 6.0f, 5.0f);
    }

    // Read-out on the info row: the longest wording that still fits beside the file info.
    const double sr = sample->sampleRate;
    const juce::String s0 (juce::String ((double) regionStart / sr, 3)), s1 (juce::String ((double) regionEnd / sr, 3)),
                       len (juce::String ((double) (regionEnd - regionStart) / sr, 3));

    const juce::String candidates[] = {
        full ? juce::String ("drag to select | double-click to reset") : "selection " + s0 + " - " + s1 + " s  (" + len + " s)",
        full ? juce::String ("drag to select")                         : s0 + " - " + s1 + " s  (" + len + " s)",
        full ? juce::String()                                          : s0 + " - " + s1 + " s"
    };

    const juce::Font font { juce::FontOptions (13.0f) };
    const int available = (int) area.getWidth() - waveform.getInfoWidth() - 24;

    for (const auto& text : candidates)
    {
        if (text.isNotEmpty() && juce::GlyphArrangement::getStringWidthInt (font, text) <= available)
        {
            g.setFont (font);
            g.setColour (full ? theme::subtle.withAlpha (0.7f) : theme::accent);
            g.drawText (text, juce::Rectangle<float> (area.getX(), area.getY() - 20.0f, area.getWidth(), 18.0f),
                        juce::Justification::centredRight, false);
            break;
        }
    }
}

// ---- mouse --------------------------------------------------------------------
void SelectionOverlay::mouseMove (const juce::MouseEvent& e)
{
    if (sample == nullptr)
        return;

    const auto edge = region::hitEdge (e.position.x, xOf (regionStart), xOf (regionEnd), kEdgeTolerance);
    setMouseCursor (edge != region::Edge::None ? juce::MouseCursor::LeftRightResizeCursor
                                               : juce::MouseCursor::NormalCursor);
}

void SelectionOverlay::mouseDown (const juce::MouseEvent& e)
{
    drag = Drag::None;

    if (sample == nullptr || numFrames() < 2)
        return;

    const auto area = waveform.getWaveArea();
    const auto edge = region::hitEdge (e.position.x, xOf (regionStart), xOf (regionEnd), kEdgeTolerance);

    if (edge == region::Edge::None && ! area.expanded (kEdgeTolerance, 6.0f).contains (e.position))
        return;

    savedStart = regionStart;
    savedEnd = regionEnd;

    if (edge == region::Edge::Start)       drag = Drag::Start;
    else if (edge == region::Edge::End)    drag = Drag::End;
    else
    {
        drag = Drag::NewRegion;
        anchorFrame = frameAt (e.position.x);
    }
}

void SelectionOverlay::mouseDrag (const juce::MouseEvent& e)
{
    if (drag == Drag::None)
        return;

    const int f = frameAt (e.position.x);
    int s = savedStart, en = savedEnd;

    switch (drag)
    {
        case Drag::NewRegion: s = juce::jmin (anchorFrame, f); en = juce::jmax (anchorFrame, f); break;
        case Drag::Start:     s = juce::jmin (f, savedEnd - region::kMinFrames);   break;
        case Drag::End:       en = juce::jmax (f, savedStart + region::kMinFrames); break;
        case Drag::None:      break;
    }

    region::sanitize (s, en, numFrames());

    if (s != regionStart || en != regionEnd)
    {
        regionStart = s;
        regionEnd = en;
        repaint();
    }
}

void SelectionOverlay::mouseUp (const juce::MouseEvent& e)
{
    if (drag == Drag::None)
        return;

    const bool wasNew = (drag == Drag::NewRegion);
    drag = Drag::None;

    if (wasNew && e.getDistanceFromDragStart() < kClickSlop)    // a plain click: keep the old region
    {
        regionStart = savedStart;
        regionEnd = savedEnd;
        repaint();
        return;
    }

    if ((regionStart != savedStart || regionEnd != savedEnd) && onCommit)
        onCommit (regionStart, regionEnd);
}

void SelectionOverlay::mouseDoubleClick (const juce::MouseEvent&)
{
    if (sample != nullptr && onCommit)
        onCommit (0, numFrames());
}
