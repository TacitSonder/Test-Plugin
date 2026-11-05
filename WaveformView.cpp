#include "WaveformView.h"
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

    path.clear();

    const float margin = (float) juce::jlimit (6, 16, juce::jmin (getWidth(), getHeight()) / 25);
    waveArea = getLocalBounds().toFloat().reduced (margin).withTrimmedTop (22.0f);

    const int w = (int) waveArea.getWidth();

    if (sample == nullptr || sample->numPeakBuckets <= 0 || w < 2 || waveArea.getHeight() < 4.0f)
        return;

    const int nb = sample->numPeakBuckets;
    const float* pk = sample->peaks.data();
    const float midY = waveArea.getCentreY();
    const float halfH = waveArea.getHeight() * 0.5f;

    columnTop.resize ((size_t) w);
    columnBottom.resize ((size_t) w);

    // Merge the stored peak buckets into one (min, max) pair per pixel column.
    for (int c = 0; c < w; ++c)
    {
        int b0 = (int) ((juce::int64) c * nb / w);
        int b1 = (int) ((juce::int64) (c + 1) * nb / w);
        b0 = juce::jmin (b0, nb - 1);
        b1 = juce::jlimit (b0 + 1, nb, b1);

        float mn = pk[b0 * 2], mx = pk[b0 * 2 + 1];

        for (int b = b0 + 1; b < b1; ++b)
        {
            mn = juce::jmin (mn, pk[b * 2]);
            mx = juce::jmax (mx, pk[b * 2 + 1]);
        }

        float top = midY - juce::jlimit (-1.0f, 1.0f, mx) * halfH;
        float bottom = midY - juce::jlimit (-1.0f, 1.0f, mn) * halfH;

        if (bottom - top < 1.0f)               // keep silence visible as a hairline
        {
            const float m = 0.5f * (top + bottom);
            top = m - 0.5f;
            bottom = m + 0.5f;
        }

        columnTop[(size_t) c] = top;
        columnBottom[(size_t) c] = bottom;
    }

    const float x0 = waveArea.getX();
    path.preallocateSpace (6 * w + 8);
    path.startNewSubPath (x0, columnTop[0]);

    for (int c = 1; c < w; ++c)
        path.lineTo (x0 + (float) c, columnTop[(size_t) c]);

    for (int c = w - 1; c >= 0; --c)
        path.lineTo (x0 + (float) c, columnBottom[(size_t) c]);

    path.closeSubPath();
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
