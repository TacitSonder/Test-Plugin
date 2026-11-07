#include "WaveformView.h"
#include "WaveformGeometry.h"
#include "UiTheme.h"

WaveformView::WaveformView (perf::Monitor& pm) : perfMon (pm)
{
    setOpaque (true);
    setBufferedToImage (true);
}

void WaveformView::setSample (std::shared_ptr<const SampleData> newSample)
{
    if (newSample == sample)
        return;

    sample = std::move (newSample);

    if (sample != nullptr)
        info = sample->name + "   |   " + juce::String (sample->getDurationSeconds(), 2) + " s   |   "
             + juce::String (sample->sampleRate / 1000.0, 1) + " kHz   |   "
             + (sample->numChannels == 1 ? "mono" : "stereo");
    else
        info = {};

    infoWidth = juce::GlyphArrangement::getStringWidthInt (juce::Font (juce::FontOptions (13.0f)), info);
    rebuildPath();
    repaint();
}

void WaveformView::setDropHighlight (bool shouldHighlight)
{
    if (dropHighlight != shouldHighlight)
    {
        dropHighlight = shouldHighlight;
        repaint();
    }
}

void WaveformView::resized()
{
    rebuildPath();
}

void WaveformView::rebuildPath()
{
    perf::ScopedTimer timer (perfMon, perf::Id::WaveformPath);

    const float margin = (float) juce::jlimit (6, 16, juce::jmin (getWidth(), getHeight()) / 25);
    waveArea = getLocalBounds().toFloat().reduced (margin).withTrimmedTop (22.0f);

    path.clear();

    if (sample != nullptr)
        waveform::buildOutline (*sample, waveArea, path, columnTop, columnBottom);
}

void WaveformView::paint (juce::Graphics& g)
{
    perf::ScopedTimer timer (perfMon, perf::Id::WaveformPaint);

    g.fillAll (theme::background);

    const auto bounds = getLocalBounds().toFloat();
    g.setColour (theme::panel);
    g.fillRoundedRectangle (bounds.reduced (1.0f), 6.0f);
    g.setColour (dropHighlight ? theme::accent : theme::outline);
    g.drawRoundedRectangle (bounds.reduced (dropHighlight ? 1.5f : 0.5f), 6.0f, dropHighlight ? 2.5f : 1.0f);

    if (sample == nullptr)
    {
        g.setColour (dropHighlight ? theme::accent : theme::subtle);
        g.setFont (juce::FontOptions (juce::jlimit (14.0f, 26.0f, (float) getHeight() / 12.0f)));
        g.drawFittedText ("Drop up to 8 audio files here", getLocalBounds().reduced (20),
                          juce::Justification::centred, 2);
        return;
    }

    const float margin = waveArea.getX();
    g.setColour (theme::subtle);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (info, juce::Rectangle<float> (margin, margin - 2.0f, bounds.getWidth() - 2.0f * margin, 18.0f),
                juce::Justification::centredLeft, true);

    g.setColour (theme::outline);
    g.drawHorizontalLine ((int) waveArea.getCentreY(), waveArea.getX(), waveArea.getRight());

    g.setColour (theme::accent);
    g.fillPath (path);
}
