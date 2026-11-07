#include "WaveformGeometry.h"

namespace waveform
{
    void buildOutline (const SampleData& sample, juce::Rectangle<float> area, juce::Path& outline,
                       std::vector<float>& columnTop, std::vector<float>& columnBottom)
    {
        outline.clear();

        const int w = (int) area.getWidth();

        if (sample.numPeakBuckets <= 0 || w < 2 || area.getHeight() < 4.0f)
            return;

        const int nb = sample.numPeakBuckets;
        const float* pk = sample.peaks.data();
        const float midY = area.getCentreY();
        const float halfH = area.getHeight() * 0.5f;

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

        const float x0 = area.getX();
        outline.preallocateSpace (6 * w + 8);
        outline.startNewSubPath (x0, columnTop[0]);

        for (int c = 1; c < w; ++c)
            outline.lineTo (x0 + (float) c, columnTop[(size_t) c]);

        for (int c = w - 1; c >= 0; --c)
            outline.lineTo (x0 + (float) c, columnBottom[(size_t) c]);

        outline.closeSubPath();
    }
}
