#include "TimelineView.h"
#include "UiTheme.h"
#include "WaveformGeometry.h"

TimelineView::TimelineView (perf::Monitor& pm) : perfMon (pm)
{
    setOpaque (true);
}

void TimelineView::setSample (std::shared_ptr<const SampleData> newSample)
{
    const bool sampleChanged = newSample != sample;
    sample = std::move (newSample);

    int s = 0, e = 0;
    float d = 0.0f;

    if (sample != nullptr)
    {
        sample->getRegion (s, e);
        d = sample->delayMs.load (std::memory_order_relaxed);
    }

    const bool regionChanged = sampleChanged || s != regionStart || e != regionEnd;
    regionStart = s;
    regionEnd = e;

    if (! dragging)
        delayMs = d;

    if (regionChanged)
        rebuildPath();

    repaint();
}

void TimelineView::setScaleMs (int ms)
{
    ms = juce::jlimit (1, 100000, ms);

    if (ms != scaleMs)
    {
        scaleMs = ms;
        rebuildPath();
        repaint();
    }
}

void TimelineView::resized()
{
    const float margin = (float) juce::jlimit (6, 14, juce::jmin (getWidth(), getHeight()) / 20);
    auto b = getLocalBounds().toFloat().reduced (margin);

    rulerArea  = b.removeFromTop (20.0f);
    handleArea = b.removeFromBottom (14.0f);
    b.removeFromBottom (5.0f);
    waveArea = b;

    rebuildPath();
}

void TimelineView::rebuildPath()
{
    perf::ScopedTimer timer (perfMon, perf::Id::TimelinePath);

    path.clear();

    if (sample != nullptr)
        waveform::buildTimelineOutline (*sample, regionStart, regionEnd, (double) scaleMs, waveArea,
                                        path, columnTop, columnBottom);
}

float TimelineView::thumbX() const noexcept
{
    return juce::jlimit (waveArea.getX(), waveArea.getRight() - kThumbWidth, msToX ((double) delayMs));
}

// ---- drawing ------------------------------------------------------------------
void TimelineView::paint (juce::Graphics& g)
{
    perf::ScopedTimer timer (perfMon, perf::Id::TimelinePaint);

    const auto clip = g.getClipBounds().toFloat();

    if (clip.intersects (rulerArea))
    {
        g.fillAll (theme::background);
        const auto bounds = getLocalBounds().toFloat();
        g.setColour (theme::panel);
        g.fillRoundedRectangle (bounds.reduced (1.0f), 6.0f);
        g.setColour (theme::outline);
        g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);

        // ruler: ticks at 1/2/5 x 10^n ms, at least 70 px apart
        const double step = waveform::niceTickStepMs ((double) scaleMs, (double) waveArea.getWidth(), 70.0);
        g.setFont (juce::FontOptions (12.0f));

        for (double t = 0.0; t <= (double) scaleMs + 1.0e-6; t += step)
        {
            const float x = msToX (t);
            g.setColour (theme::outline);
            g.fillRect (x - 0.5f, rulerArea.getBottom() - 6.0f, 1.0f, 6.0f);

            if (x + 28.0f < waveArea.getRight() - 26.0f)          // leave room for the unit label
            {
                g.setColour (theme::subtle);
                g.drawText (juce::String (juce::roundToInt (t)), juce::Rectangle<float> (x - 28.0f, rulerArea.getY(), 56.0f, rulerArea.getHeight() - 6.0f),
                            juce::Justification::centred, false);
            }
        }

        g.setColour (theme::subtle);
        g.drawText ("ms", rulerArea.withLeft (rulerArea.getRight() - 26.0f), juce::Justification::centredRight, false);
    }

    // waveform, shifted by the delay and clipped to its area
    {
        const juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (waveArea.getSmallestIntegerContainer());
        g.fillAll (theme::panel);

        g.setColour (theme::outline);
        g.fillRect (waveArea.getX(), waveArea.getCentreY(), waveArea.getWidth(), 1.0f);

        if (! path.isEmpty())
        {
            g.setColour (theme::accent);
            g.fillPath (path, juce::AffineTransform::translation (msToX ((double) delayMs) - waveArea.getX(), 0.0f));
        }

        g.setColour (juce::Colours::white.withAlpha (0.9f));              // trigger moment (t = 0)
        g.fillRect (waveArea.getX(), waveArea.getY(), 1.5f, waveArea.getHeight());
    }

    // delay handle
    g.setColour (theme::background);
    g.fillRoundedRectangle (handleArea, 7.0f);
    g.setColour (theme::outline);
    g.drawRoundedRectangle (handleArea.reduced (0.5f), 7.0f, 1.0f);

    if (sample != nullptr)
    {
        const float tx = thumbX();
        g.setColour (dragging ? theme::accent : theme::accent.withAlpha (0.75f));
        g.fillRoundedRectangle (juce::Rectangle<float> (tx, handleArea.getY(), kThumbWidth, handleArea.getHeight()).reduced (1.0f), 6.0f);

        g.setColour (theme::subtle);
        g.setFont (juce::FontOptions (12.0f));
        const juce::String text ("delay " + juce::String (juce::roundToInt (delayMs)) + " ms");
        const bool right = tx + kThumbWidth + 80.0f < handleArea.getRight();
        g.drawText (text, juce::Rectangle<float> (right ? tx + kThumbWidth + 6.0f : tx - 86.0f, handleArea.getY(), 80.0f, handleArea.getHeight()),
                    right ? juce::Justification::centredLeft : juce::Justification::centredRight, false);
    }
}

// ---- mouse --------------------------------------------------------------------
void TimelineView::dragTo (float mouseX)
{
    const double maxMs = juce::jmin ((double) SampleData::kMaxDelayMs,
                                     (double) scaleMs * (1.0 - (double) kThumbWidth / (double) waveArea.getWidth()));
    const double ms = (double) (mouseX - grabOffset - waveArea.getX()) / (double) waveArea.getWidth() * (double) scaleMs;
    const float rounded = (float) juce::roundToInt (juce::jlimit (0.0, juce::jmax (0.0, maxMs), ms));

    if (std::abs (rounded - delayMs) > 0.5f)       // delay is whole milliseconds
    {
        delayMs = rounded;
        repaint (waveArea.getUnion (handleArea).getSmallestIntegerContainer());

        if (onDelayChanged)
            onDelayChanged (delayMs, false);
    }
}

void TimelineView::mouseDown (const juce::MouseEvent& e)
{
    if (sample == nullptr || ! handleArea.expanded (0.0f, 4.0f).contains (e.position))
        return;

    const float tx = thumbX();
    dragging = true;
    grabOffset = (e.position.x >= tx && e.position.x <= tx + kThumbWidth) ? e.position.x - tx : kThumbWidth * 0.5f;
    dragTo (e.position.x);
}

void TimelineView::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging)
        dragTo (e.position.x);
}

void TimelineView::mouseUp (const juce::MouseEvent&)
{
    if (! dragging)
        return;

    dragging = false;

    if (onDelayChanged)
        onDelayChanged (delayMs, true);
}

void TimelineView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (sample == nullptr || ! handleArea.expanded (0.0f, 4.0f).contains (e.position))
        return;

    delayMs = 0.0f;
    repaint (waveArea.getUnion (handleArea).getSmallestIntegerContainer());

    if (onDelayChanged)
        onDelayChanged (0.0f, true);
}
