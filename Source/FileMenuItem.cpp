#include "FileMenuItem.h"
#include "UiTheme.h"

FileMenuItem::FileMenuItem (juce::String l, std::function<void()> cb)
    : label (std::move (l)), onRemove (std::move (cb))
{
}

void FileMenuItem::getIdealSize (int& idealWidth, int& idealHeight)
{
    idealWidth = juce::GlyphArrangement::getStringWidthInt (juce::Font (juce::FontOptions (15.0f)), label)
                 + 24 + kCloseZoneWidth;
    idealHeight = kRowHeight;
}

void FileMenuItem::paint (juce::Graphics& g)
{
    const bool highlighted = isItemHighlighted();

    if (highlighted)
    {
        g.setColour (findColour (juce::PopupMenu::highlightedBackgroundColourId));
        g.fillRect (getLocalBounds());
    }

    auto r = getLocalBounds();
    const auto closeZone = r.removeFromRight (kCloseZoneWidth);

    g.setColour (findColour (highlighted ? juce::PopupMenu::highlightedTextColourId
                                         : juce::PopupMenu::textColourId));
    g.setFont (juce::FontOptions (15.0f));
    g.drawText (label, r.withTrimmedLeft (12).withTrimmedRight (6), juce::Justification::centredLeft, true);

    // the "x": a round hover background and two crossing lines
    const auto c = closeZone.toFloat().withSizeKeepingCentre (20.0f, 20.0f);

    if (overClose)
    {
        g.setColour (theme::danger.withAlpha (0.85f));
        g.fillEllipse (c);
    }

    g.setColour (overClose ? juce::Colours::white : theme::subtle);
    const auto m = c.reduced (6.5f);
    g.drawLine (m.getX(), m.getY(), m.getRight(), m.getBottom(), 1.6f);
    g.drawLine (m.getX(), m.getBottom(), m.getRight(), m.getY(), 1.6f);
}

void FileMenuItem::mouseEnter (const juce::MouseEvent& e)
{
    mouseMove (e);
}

void FileMenuItem::mouseMove (const juce::MouseEvent& e)
{
    const bool now = getCloseZone().contains (e.getPosition());

    if (now != overClose)
    {
        overClose = now;
        repaint();
    }
}

void FileMenuItem::mouseExit (const juce::MouseEvent&)
{
    if (overClose)
    {
        overClose = false;
        repaint();
    }
}

void FileMenuItem::mouseDown (const juce::MouseEvent& e)
{
    if (! getCloseZone().contains (e.getPosition()))
        return;                                // normal click: the menu selects this row

    // Close the menu *now* (before the mouse-up that would select the row), then
    // let the owner remove the file.
    auto callback = onRemove;
    juce::PopupMenu::dismissAllActiveMenus();

    if (callback)
        callback();
}
