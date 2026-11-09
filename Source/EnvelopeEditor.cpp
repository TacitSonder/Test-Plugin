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
    beforeReset.reset();
    repaint();
}

void EnvelopeEditor::resized()
{
    area = getLocalBounds().toFloat().reduced (9.0f);
}

juce::Point<float> EnvelopeEditor::toPixel (mod::Point p) const noexcept
{
    return { area.getX() + (p.x - vx0) / (vx1 - vx0) * area.getWidth(),
             area.getBottom() - (p.y - vy0) / (vy1 - vy0) * area.getHeight() };
}

mod::Point EnvelopeEditor::fromPixel (juce::Point<float> px) const noexcept
{
    return { juce::jlimit (0.0f, 1.0f, vx0 + (px.x - area.getX()) / area.getWidth() * (vx1 - vx0)),
             juce::jlimit (0.0f, 1.0f, vy0 + (area.getBottom() - px.y) / area.getHeight() * (vy1 - vy0)) };
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

    // grid: 0.25 apart when fully shown, finer when zoomed in
    auto gridStep = [] (float range) { for (const float s : { 0.25f, 0.1f, 0.05f, 0.025f, 0.01f }) if (s <= range * 0.26f) return s; return 0.005f; };
    const float sx = gridStep (vx1 - vx0), sy = gridStep (vy1 - vy0);
    const bool zoomed = isZoomed();
    g.setFont (juce::FontOptions (10.0f));
    const int labelEvery = juce::jmax (1, (int) std::ceil (36.0f / (sx / (vx1 - vx0) * area.getWidth())));

    for (int k = (int) std::ceil (vx0 / sx - 1.0e-4f); (float) k * sx <= vx1 + 1.0e-4f; ++k)
    {
        const float x = toPixel ({ (float) k * sx, 0.0f }).x;
        g.setColour (theme::outline.withAlpha (0.6f));
        g.fillRect (x - 0.5f, area.getY(), 1.0f, area.getHeight());

        if (zoomed && k % labelEvery == 0)                                     // only as many labels as fit
        {
            g.setColour (theme::subtle);
            g.drawText (juce::String (juce::roundToInt ((float) k * sx * 100.0f)) + "%",
                        juce::Rectangle<float> (x - 22.0f, area.getBottom() - 11.0f, 44.0f, 11.0f), juce::Justification::centred, false);
        }
    }

    for (int k = (int) std::ceil (vy0 / sy - 1.0e-4f); (float) k * sy <= vy1 + 1.0e-4f; ++k)
    {
        const float y = toPixel ({ 0.0f, (float) k * sy }).y;
        g.setColour (theme::outline.withAlpha (0.6f));
        g.fillRect (area.getX(), y - 0.5f, area.getWidth(), 1.0f);

        if (zoomed && yLabel)
        {
            g.setColour (theme::subtle);
            g.drawText (yLabel ((float) k * sy), juce::Rectangle<float> (area.getX() + 2.0f, y - 11.0f, 44.0f, 11.0f),
                        juce::Justification::centredLeft, false);
        }
    }

    g.setColour (theme::outline);
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 4.0f, 1.0f);

    {
        const juce::Graphics::ScopedSaveState state (g);                       // points and lines stay inside the frame
        g.reduceClipRegion (area.expanded (kPointRadius + 1.0f).getSmallestIntegerContainer());

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

    if (dragIndex >= 0)                                                        // exact values while dragging
    {
        const auto& p = env.pts[(size_t) dragIndex];
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText ("x " + juce::String (juce::roundToInt (p.x * 100.0f)) + "%   y " + (yLabel ? yLabel (p.y) : juce::String (p.y, 2)),
                    area.withHeight (14.0f), juce::Justification::centredRight, false);
    }
}

void EnvelopeEditor::edited()
{
    beforeReset.reset();                      // "Undo reset" only applies right after a reset
    repaint();

    if (onChanged)
        onChanged (env);
}

void EnvelopeEditor::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        dragIndex = -1;
        showMenu();
        return;
    }

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

void EnvelopeEditor::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const bool sideways = std::abs (w.deltaX) > std::abs (w.deltaY);
    const float d = sideways ? w.deltaX : w.deltaY;
    const float fx = juce::jlimit (0.0f, 1.0f, (e.position.x - area.getX()) / area.getWidth());
    const float fy = juce::jlimit (0.0f, 1.0f, (area.getBottom() - e.position.y) / area.getHeight());

    if (e.mods.isAltDown())                         mod::panRange (vy0, vy1, d * (vy1 - vy0) * 0.5f);
    else if (sideways || e.mods.isShiftDown())      mod::panRange (vx0, vx1, -d * (vx1 - vx0) * 0.5f);
    else if (e.mods.isCtrlDown() || e.mods.isCommandDown())
                                                    mod::zoomRange (vy0, vy1, fy, std::exp (-d * 0.8f));
    else                                            mod::zoomRange (vx0, vx1, fx, std::exp (-d * 0.8f));

    repaint();
}

void EnvelopeEditor::showMenu()
{
    juce::PopupMenu menu;
    menu.addItem (1, "Reset envelope", (bool) defaultEnvelope);
    menu.addItem (2, "Undo reset", beforeReset.has_value());
    menu.addSeparator();
    menu.addItem (3, "Reset zoom", isZoomed());

    menu.showMenuAsync (juce::PopupMenu::Options(), [safe = juce::Component::SafePointer<EnvelopeEditor> (this)] (int result)
    {
        if (safe == nullptr)
            return;

        if (result == 1 && safe->defaultEnvelope)
        {
            safe->beforeReset = safe->env;
            safe->env = safe->defaultEnvelope();
            safe->repaint();
            if (safe->onChanged) safe->onChanged (safe->env);
        }
        else if (result == 2 && safe->beforeReset.has_value())
        {
            safe->env = *safe->beforeReset;
            safe->beforeReset.reset();
            safe->repaint();
            if (safe->onChanged) safe->onChanged (safe->env);
        }
        else if (result == 3)
        {
            safe->vx0 = safe->vy0 = 0.0f;
            safe->vx1 = safe->vy1 = 1.0f;
            safe->repaint();
        }
    });
}
