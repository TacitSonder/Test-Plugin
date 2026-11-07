#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

// One row of the file dropdown: "3:  kick.wav" with an "x" at the right end.
// Clicking the row selects the file (handled by the popup menu itself);
// clicking the x closes the menu and calls onRemove instead.
class FileMenuItem : public juce::PopupMenu::CustomComponent
{
public:
    FileMenuItem (juce::String label, std::function<void()> onRemoveClicked);

    void getIdealSize (int& idealWidth, int& idealHeight) override;
    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;

    static constexpr int kRowHeight = 28;
    static constexpr int kCloseZoneWidth = 34;

private:
    juce::Rectangle<int> getCloseZone() const noexcept
    {
        return getLocalBounds().removeFromRight (kCloseZoneWidth);
    }

    juce::String label;
    std::function<void()> onRemove;
    bool overClose = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FileMenuItem)
};
