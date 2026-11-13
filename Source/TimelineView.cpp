#include "TimelineView.h"
#include "UiTheme.h"
#include "WaveformGeometry.h"

TimelineView::TimelineView (perf::Monitor& pm) : perfMon (pm)
{
    setOpaque (true);

    for (int i = 0; i < layercfg::kCount; ++i)
    {
        layers[(size_t) i].name = layercfg::kNames[i];
        layers[(size_t) i].colour = theme::layerColour (i);
    }
}

void TimelineView::setLayerStyle (int layer, const juce::String& name, juce::Colour colour)
{
    layers[(size_t) layer].name = name;
    layers[(size_t) layer].colour = colour;
    repaint();
}

void TimelineView::setSample (int layer, std::shared_ptr<const SampleData> newSample)
{
    auto& l = layers[(size_t) layer];
    const bool sampleChanged = newSample != l.sample;
    l.sample = std::move (newSample);

    int s = 0, e = 0;
    float d = 0.0f;

    if (l.sample != nullptr)
    {
        l.sample->getRegion (s, e);
        d = l.sample->delayMs.load (std::memory_order_relaxed);
    }

    const bool regionChanged = sampleChanged || s != l.regionStart || e != l.regionEnd;
    l.regionStart = s;
    l.regionEnd = e;

    if (dragLayer != layer)                       // keep the local value while this layer's handle is being dragged
        l.delayMs = d;

    if (regionChanged)
        rebuildPath (l);

    repaint();
}

void TimelineView::setActiveLayer (int layer)
{
    if (layer != activeLayer)
    {
        activeLayer = layer;
        repaint();
    }
}

void TimelineView::setScaleMs (int ms)
{
    ms = juce::jlimit (1, 100000, ms);

    if (ms != scaleMs)
    {
        scaleMs = ms;
        for (auto& l : layers)
            rebuildPath (l);
        repaint();
    }
}

void TimelineView::resized()
{
    const float margin = (float) juce::jlimit (6, 14, juce::jmin (getWidth(), getHeight()) / 20);
    auto b = getLocalBounds().toFloat().reduced (margin);

    rulerArea = b.removeFromTop (20.0f);

    for (int i = layercfg::kCount - 1; i >= 0; --i)          // handle rows at the bottom, layer 0 on top
    {
        rows[(size_t) i] = b.removeFromBottom (kRowHeight);
        b.removeFromBottom (kRowGap);
    }

    waveArea = b;

    for (auto& l : layers)
        rebuildPath (l);
}

void TimelineView::rebuildPath (LayerView& l)
{
    perf::ScopedTimer timer (perfMon, perf::Id::TimelinePath);

    l.path.clear();

    if (l.sample != nullptr)
        waveform::buildTimelineOutline (*l.sample, l.regionStart, l.regionEnd, (double) scaleMs, waveArea,
                                        l.path, columnTop, columnBottom);
}

float TimelineView::thumbX (const LayerView& l) const noexcept
{
    return juce::jlimit (waveArea.getX(), waveArea.getRight() - kThumbWidth, msToX ((double) l.delayMs));
}

juce::Rectangle<int> TimelineView::dirtyArea() const noexcept
{
    return waveArea.getUnion (rows.front()).getUnion (rows.back()).getSmallestIntegerContainer();
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

    // waveforms of all layers, each shifted by its own delay; the active layer is drawn last (on top)
    {
        const juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (waveArea.getSmallestIntegerContainer());
        g.fillAll (theme::panel);

        g.setColour (theme::outline);
        g.fillRect (waveArea.getX(), waveArea.getCentreY(), waveArea.getWidth(), 1.0f);

        for (int pass = 0; pass < layercfg::kCount; ++pass)
        {
            const int i = pass == layercfg::kCount - 1 ? activeLayer : (pass >= activeLayer ? pass + 1 : pass);
            const auto& l = layers[(size_t) i];

            if (! l.path.isEmpty())
            {
                g.setColour (l.colour.withAlpha (i == activeLayer ? 0.95f : 0.7f));
                g.fillPath (l.path, juce::AffineTransform::translation (msToX ((double) l.delayMs) - waveArea.getX(), 0.0f));
            }
        }

        g.setColour (juce::Colours::white.withAlpha (0.9f));              // trigger moment (t = 0)
        g.fillRect (waveArea.getX(), waveArea.getY(), 1.5f, waveArea.getHeight());

        // legend: a coloured chip per layer, bottom left
        g.setFont (juce::FontOptions (11.0f));
        for (int i = 0; i < layercfg::kCount; ++i)
        {
            const auto chip = juce::Rectangle<float> (waveArea.getX() + 6.0f, waveArea.getBottom() - (float) (layercfg::kCount - i) * 17.0f - 2.0f, 60.0f, 15.0f);
            g.setColour (layers[(size_t) i].colour.withAlpha (i == activeLayer ? 1.0f : 0.75f));
            g.fillRoundedRectangle (chip, 4.0f);
            g.setColour (theme::textOn (layers[(size_t) i].colour));
            g.drawText (layers[(size_t) i].name, chip, juce::Justification::centred, false);
        }
    }

    // one delay handle per layer
    for (int i = 0; i < layercfg::kCount; ++i)
    {
        const auto& l = layers[(size_t) i];
        const auto& row = rows[(size_t) i];

        g.setColour (theme::background);
        g.fillRoundedRectangle (row, 7.0f);
        g.setColour (theme::outline);
        g.drawRoundedRectangle (row.reduced (0.5f), 7.0f, 1.0f);

        if (l.sample != nullptr)
        {
            const float tx = thumbX (l);
            g.setColour (dragLayer == i || i == activeLayer ? l.colour : l.colour.withAlpha (0.7f));
            g.fillRoundedRectangle (juce::Rectangle<float> (tx, row.getY(), kThumbWidth, row.getHeight()).reduced (1.0f), 6.0f);

            g.setColour (theme::subtle);
            g.setFont (juce::FontOptions (12.0f));
            const juce::String text (l.name + "  delay " + juce::String (juce::roundToInt (l.delayMs)) + " ms");
            const bool right = tx + kThumbWidth + 150.0f < row.getRight();
            g.drawText (text, juce::Rectangle<float> (right ? tx + kThumbWidth + 6.0f : tx - 156.0f, row.getY(), 150.0f, row.getHeight()),
                        right ? juce::Justification::centredLeft : juce::Justification::centredRight, false);
        }
    }
}

// ---- mouse --------------------------------------------------------------------
int TimelineView::rowAt (juce::Point<float> p) const noexcept
{
    for (int i = 0; i < layercfg::kCount; ++i)
        if (layers[(size_t) i].sample != nullptr && rows[(size_t) i].expanded (0.0f, 2.0f).contains (p))
            return i;

    return -1;
}

void TimelineView::dragTo (float mouseX)
{
    auto& l = layers[(size_t) dragLayer];
    const double maxMs = juce::jmin ((double) SampleData::kMaxDelayMs,
                                     (double) scaleMs * (1.0 - (double) kThumbWidth / (double) waveArea.getWidth()));
    const double ms = (double) (mouseX - grabOffset - waveArea.getX()) / (double) waveArea.getWidth() * (double) scaleMs;
    const float rounded = (float) juce::roundToInt (juce::jlimit (0.0, juce::jmax (0.0, maxMs), ms));

    if (std::abs (rounded - l.delayMs) > 0.5f)       // delay is whole milliseconds
    {
        l.delayMs = rounded;
        repaint (dirtyArea());

        if (onDelayChanged)
            onDelayChanged (dragLayer, l.delayMs, false);
    }
}

void TimelineView::mouseDown (const juce::MouseEvent& e)
{
    const int i = rowAt (e.position);

    if (i < 0)
        return;

    dragLayer = i;
    const float tx = thumbX (layers[(size_t) i]);
    grabOffset = (e.position.x >= tx && e.position.x <= tx + kThumbWidth) ? e.position.x - tx : kThumbWidth * 0.5f;
    dragTo (e.position.x);
}

void TimelineView::mouseDrag (const juce::MouseEvent& e)
{
    if (dragLayer >= 0)
        dragTo (e.position.x);
}

void TimelineView::mouseUp (const juce::MouseEvent&)
{
    if (dragLayer < 0)
        return;

    const int i = dragLayer;
    dragLayer = -1;

    if (onDelayChanged)
        onDelayChanged (i, layers[(size_t) i].delayMs, true);
}

void TimelineView::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int i = rowAt (e.position);

    if (i < 0)
        return;

    layers[(size_t) i].delayMs = 0.0f;
    repaint (dirtyArea());

    if (onDelayChanged)
        onDelayChanged (i, 0.0f, true);
}
