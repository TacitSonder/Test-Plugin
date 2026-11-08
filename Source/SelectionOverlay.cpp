#include "SelectionOverlay.h"
#include "UiTheme.h"

namespace
{
    constexpr float kEdgeTolerance = 7.0f;   // pixels
    constexpr float kClickSlop = 3.0f;       // movement below this is a click, not a drag
}

SelectionOverlay::SelectionOverlay (perf::Monitor& pm, WaveformView& w) : perfMon (pm), waveform (w)
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
    return a.getX() + (float) (((double) frame - waveform.getViewStart()) / waveform.getViewLength()) * a.getWidth();
}

int SelectionOverlay::frameAt (float x) const noexcept
{
    const auto a = waveform.getWaveArea();
    const double f = waveform.getViewStart() + (double) (x - a.getX()) / (double) a.getWidth() * waveform.getViewLength();
    return juce::jlimit (0, numFrames(), (int) std::llround (f));
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
    const float cs = juce::jlimit (area.getX(), area.getRight(), xs), ce = juce::jlimit (area.getX(), area.getRight(), xe);
    const bool full = region::isFull (regionStart, regionEnd, numFrames());

    if (! full)
    {
        g.setColour (juce::Colours::black.withAlpha (0.55f));                     // dim what will not play
        g.fillRect (area.getX(), area.getY(), cs - area.getX(), area.getHeight());
        g.fillRect (ce, area.getY(), area.getRight() - ce, area.getHeight());

        g.setColour (theme::accent.withAlpha (0.10f));                            // tint what will play
        g.fillRect (cs, area.getY(), ce - cs, area.getHeight());
    }

    // edges (only when inside the visible part) with a small grip on top
    g.setColour (full ? theme::subtle.withAlpha (0.55f) : theme::accent);
    for (const float x : { xs, xe })
    {
        if (x < area.getX() - 0.5f || x > area.getRight() + 0.5f)
            continue;

        g.fillRect (x - 0.5f, area.getY(), 1.0f, area.getHeight());
        g.fillRect (x - 3.0f, area.getY(), 6.0f, 5.0f);
    }

    const bool zoomed = waveform.isZoomed();

    if (zoomed)                                                                    // where the view is within the file
    {
        const float n = (float) numFrames();
        const float x0 = area.getX() + area.getWidth() * (float) waveform.getViewStart() / n;
        const float x1 = area.getX() + area.getWidth() * (float) (waveform.getViewStart() + waveform.getViewLength()) / n;
        g.setColour (theme::subtle.withAlpha (0.25f));
        g.fillRect (area.getX(), area.getBottom() - 4.0f, area.getWidth(), 3.0f);
        g.setColour (theme::accent.withAlpha (0.8f));
        g.fillRect (x0, area.getBottom() - 4.0f, juce::jmax (3.0f, x1 - x0), 3.0f);
    }

    // Read-out on the info row: the longest wording that still fits beside the file info.
    const double sr = sample->sampleRate;
    const juce::String s0 (juce::String ((double) regionStart / sr, 3)), s1 (juce::String ((double) regionEnd / sr, 3)),
                       len (juce::String ((double) (regionEnd - regionStart) / sr, 3));
    const juce::String zoom = zoomed ? "   |   zoom x" + juce::String ((double) numFrames() / waveform.getViewLength(), 1) : juce::String();

    const juce::String candidates[] = {
        full ? juce::String ("drag: select  |  wheel: zoom  |  double-click: reset") : "selection " + s0 + " - " + s1 + " s  (" + len + " s)" + zoom,
        full ? juce::String ("drag: select  |  wheel: zoom" ) + zoom                   : s0 + " - " + s1 + " s  (" + len + " s)" + zoom,
        full ? juce::String ("drag to select")                                          : s0 + " - " + s1 + " s"
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

    const float xs = xOf (regionStart), xe = xOf (regionEnd);
    const auto edge = region::hitEdge (e.position.x, xs, xe, kEdgeTolerance);
    const bool inside = ! region::isFull (regionStart, regionEnd, numFrames()) && e.position.x > xs && e.position.x < xe;

    setMouseCursor (edge != region::Edge::None ? juce::MouseCursor::LeftRightResizeCursor
                    : inside                   ? juce::MouseCursor::DraggingHandCursor
                                               : juce::MouseCursor::NormalCursor);
}

void SelectionOverlay::mouseDown (const juce::MouseEvent& e)
{
    drag = Drag::None;

    if (sample == nullptr || numFrames() < 2)
        return;

    if (e.mods.isPopupMenu())
    {
        showMenu();
        return;
    }

    const auto area = waveform.getWaveArea();
    const float xs = xOf (regionStart), xe = xOf (regionEnd);
    const auto edge = region::hitEdge (e.position.x, xs, xe, kEdgeTolerance);
    const bool full = region::isFull (regionStart, regionEnd, numFrames());

    if (edge == region::Edge::None && ! area.expanded (kEdgeTolerance, 6.0f).contains (e.position))
        return;

    savedStart = regionStart;
    savedEnd = regionEnd;
    anchorFrame = frameAt (e.position.x);

    if (edge == region::Edge::Start)                                  drag = Drag::Start;
    else if (edge == region::Edge::End)                               drag = Drag::End;
    else if (! full && e.position.x > xs && e.position.x < xe)        drag = Drag::Move;
    else                                                              drag = Drag::NewRegion;
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
        case Drag::Move:      region::shift (savedStart, savedEnd, f - anchorFrame, numFrames(), s, en); break;
        case Drag::None:      break;
    }

    if (drag != Drag::Move)
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

void SelectionOverlay::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (sample == nullptr || drag != Drag::None)
        return;

    const bool sideways = std::abs (w.deltaX) > std::abs (w.deltaY);
    const double delta = sideways ? (double) w.deltaX : (double) w.deltaY;

    if (sideways || e.mods.isShiftDown())
    {
        waveform.pan (-delta * 0.5 * waveform.getViewLength());
    }
    else
    {
        const auto area = waveform.getWaveArea();
        waveform.zoomAt (juce::jlimit (0.0, 1.0, (double) ((e.position.x - area.getX()) / area.getWidth())),
                         std::exp (-delta * 0.8));
    }

    repaint();
}

void SelectionOverlay::showMenu()
{
    const bool hasSelection = ! region::isFull (regionStart, regionEnd, numFrames());

    juce::PopupMenu menu;
    menu.addItem (1, "Zoom to selection", hasSelection);
    menu.addItem (2, "Zoom to fit", waveform.isZoomed());
    menu.addSeparator();
    menu.addItem (3, "Select whole file", hasSelection);

    menu.showMenuAsync (juce::PopupMenu::Options(), [safe = juce::Component::SafePointer<SelectionOverlay> (this)] (int result)
    {
        if (safe == nullptr)
            return;

        const int n = safe->numFrames(), pad = (safe->regionEnd - safe->regionStart) / 20;

        if (result == 1)       safe->waveform.setView ((double) (safe->regionStart - pad), (double) (safe->regionEnd - safe->regionStart + 2 * pad));
        else if (result == 2)  safe->waveform.zoomToFit();
        else if (result == 3 && safe->onCommit)  safe->onCommit (0, n);

        safe->repaint();
    });
}
