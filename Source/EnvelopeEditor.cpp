#include "EnvelopeEditor.h"
#include "UiTheme.h"

namespace { constexpr float kPointRadius = 4.5f, kHitRadius = 9.0f; }

EnvelopeEditor::EnvelopeEditor (perf::Monitor& pm) : perfMon (pm)
{
    setOpaque (true);
}

void EnvelopeEditor::setEnvelope (const mod::Envelope& e)
{
    env = e;
    repaint();
}

void EnvelopeEditor::resized()
{
    area = getLocalBounds().toFloat().reduced (9.0f);
}

juce::Point<float> EnvelopeEditor::toPixel (mod::Point p) const noexcept
{
    return { area.getX() + p.x * area.getWidth(), area.getBottom() - p.y * area.getHeight() };
}

mod::Point EnvelopeEditor::fromPixel (juce::Point<float> px) const noexcept
{
    return { juce::jlimit (0.0f, 1.0f, (px.x - area.getX()) / area.getWidth()),
             juce::jlimit (0.0f, 1.0f, (area.getBottom() - px.y) / area.getHeight()) };
}

int EnvelopeEditor::hitTest (juce::Point<float> px) const noexcept
{
    for (int i = env.count - 1; i >= 0; --i)          // topmost (last) first
        if (toPixel (env.pts[(size_t) i]).getDistanceFrom (px) <= kHitRadius)
            return i;

    return -1;
}

void EnvelopeEditor::paint (juce::Graphics& g)
{
    perf::ScopedTimer timer (perfMon, perf::Id::EnvPaint);

    g.fillAll (theme::background);
    g.setColour (theme::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 4.0f);

    g.setColour (theme::outline.withAlpha (0.6f));                         // grid
    for (int i = 0; i <= 4; ++i)
    {
        const float fx = area.getX() + area.getWidth() * (float) i / 4.0f;
        const float fy = area.getY() + area.getHeight() * (float) i / 4.0f;
        g.fillRect (fx - 0.5f, area.getY(), 1.0f, area.getHeight());
        g.fillRect (area.getX(), fy - 0.5f, area.getWidth(), 1.0f);
    }

    g.setColour (theme::outline);
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 4.0f, 1.0f);

    g.setColour (theme::accent);
    for (int i = 0; i + 1 < env.count; ++i)
        g.drawLine (juce::Line<float> (toPixel (env.pts[(size_t) i]), toPixel (env.pts[(size_t) i + 1])), 1.6f);

    for (int i = 0; i < env.count; ++i)
    {
        const auto c = toPixel (env.pts[(size_t) i]);
        g.setColour (i == dragIndex ? juce::Colours::white : theme::accent);
        g.fillEllipse (c.x - kPointRadius, c.y - kPointRadius, kPointRadius * 2.0f, kPointRadius * 2.0f);
    }
}

void EnvelopeEditor::edited()
{
    repaint();

    if (onChanged)
        onChanged (env);
}

void EnvelopeEditor::mouseDown (const juce::MouseEvent& e)
{
    dragIndex = hitTest (e.position);

    if (dragIndex < 0 && area.expanded (kHitRadius).contains (e.position))
    {
        const auto p = fromPixel (e.position);
        dragIndex = env.addPoint (p.x, p.y);

        if (dragIndex >= 0)
            edited();
    }
}

void EnvelopeEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (dragIndex < 0)
        return;

    auto p = fromPixel (e.position);
    const int last = env.count - 1;

    if (dragIndex == 0)          p.x = 0.0f;                                          // ends only move vertically
    else if (dragIndex == last)  p.x = 1.0f;
    else                         p.x = juce::jlimit (env.pts[(size_t) dragIndex - 1].x, env.pts[(size_t) dragIndex + 1].x, p.x);

    if (std::abs (p.x - env.pts[(size_t) dragIndex].x) > 1.0e-6f || std::abs (p.y - env.pts[(size_t) dragIndex].y) > 1.0e-6f)
    {
        env.pts[(size_t) dragIndex] = p;
        edited();
    }
}

void EnvelopeEditor::mouseUp (const juce::MouseEvent&)
{
    dragIndex = -1;
    repaint();
}

void EnvelopeEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int i = hitTest (e.position);

    if (i > 0 && i < env.count - 1 && env.count > 2)
    {
        env.removePoint (i);
        dragIndex = -1;
        edited();
    }
}
