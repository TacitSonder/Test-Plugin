#include "PerfOverlay.h"
#include "UiTheme.h"

namespace
{
    constexpr int kLineHeight = 14;
    constexpr int kPadding = 8;
}

PerfOverlay::PerfOverlay (perf::Monitor& m, std::function<juce::String()> info) : monitor (m), extraInfo (std::move (info))
{
    setInterceptsMouseClicks (false, false);
    setVisible (false);
}

int PerfOverlay::getPreferredHeight() const noexcept
{
    return (perf::Monitor::kReportLines + 1) * kLineHeight + 2 * kPadding;
}

void PerfOverlay::setActive (bool shouldBeActive)
{
    if (shouldBeActive)
    {
        monitor.resetSession();
        monitor.buildReport();          // discard: starts a fresh measuring window
        lines.clear();
        lines.add ("collecting...");
        startTimer (500);
    }
    else
    {
        stopTimer();
    }

    setVisible (shouldBeActive);
}

void PerfOverlay::timerCallback()
{
    lines = monitor.buildReport();

    lines.add (extraInfo ? extraInfo() : juce::String());

    repaint();
}

void PerfOverlay::paint (juce::Graphics& g)
{
    g.setColour (juce::Colours::black.withAlpha (0.82f));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
    g.setColour (theme::outline);
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 6.0f, 1.0f);

    // Shrink the rows if the overlay was given less height than it would like.
    const int rows = juce::jmax (1, perf::Monitor::kReportLines + 1);
    const int lineH = juce::jlimit (10, kLineHeight, (getHeight() - 2 * kPadding) / rows);

    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), (float) lineH * 0.75f, juce::Font::plain));

    int y = kPadding;

    for (int i = 0; i < lines.size(); ++i)
    {
        const auto& line = lines[i];

        if (i == 0)                         g.setColour (monitor.getOverruns() > 0 ? theme::danger : theme::accent);
        else if (i == 1 || line.startsWith ("--")) g.setColour (theme::subtle);
        else                                g.setColour (theme::text);

        g.drawText (line, kPadding, y, getWidth() - 2 * kPadding, lineH,
                    juce::Justification::centredLeft, false);
        y += lineH;
    }
}
