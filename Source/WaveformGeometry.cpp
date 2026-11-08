#include "WaveformGeometry.h"

namespace waveform
{
    void buildOutline (const SampleData& sample, double firstFrame, int endFrame, double framesPerColumn,
                       juce::Rectangle<float> area, juce::Path& outline,
                       std::vector<float>& columnTop, std::vector<float>& columnBottom)
    {
        outline.clear();

        endFrame = juce::jmin (endFrame, sample.numFrames);     // never read past the audio
        const int w = (int) area.getWidth();

        if (w < 2 || area.getHeight() < 4.0f || framesPerColumn <= 0.0 || (double) endFrame <= firstFrame)
            return;

        const int columns = juce::jmin (w, (int) std::ceil (((double) endFrame - firstFrame) / framesPerColumn));

        if (columns < 2)
            return;

        const bool usePeaks = framesPerColumn >= 2.0 * SampleData::kPeakFrames && ! sample.peaks.empty();
        const float* l = sample.audio.getReadPointer (0);
        const float* r = sample.numChannels > 1 ? sample.audio.getReadPointer (1) : nullptr;
        const float* pk = sample.peaks.data();
        const int nb = sample.numPeakBuckets;
        const float midY = area.getCentreY();
        const float halfH = area.getHeight() * 0.5f;

        columnTop.resize ((size_t) columns);
        columnBottom.resize ((size_t) columns);

        for (int c = 0; c < columns; ++c)
        {
            int i0 = (int) (firstFrame + (double) c * framesPerColumn);
            int i1 = (int) (firstFrame + (double) (c + 1) * framesPerColumn);
            i0 = juce::jlimit (0, endFrame - 1, i0);
            i1 = juce::jlimit (i0 + 1, endFrame, i1);

            float mn, mx;

            if (usePeaks)
            {
                const int b0 = i0 / SampleData::kPeakFrames;
                const int b1 = juce::jmax (b0 + 1, juce::jmin (nb, (i1 + SampleData::kPeakFrames - 1) / SampleData::kPeakFrames));
                mn = pk[b0 * 2];
                mx = pk[b0 * 2 + 1];

                for (int b = b0 + 1; b < b1; ++b)
                {
                    mn = juce::jmin (mn, pk[b * 2]);
                    mx = juce::jmax (mx, pk[b * 2 + 1]);
                }
            }
            else
            {
                auto range = juce::FloatVectorOperations::findMinAndMax (l + i0, i1 - i0);
                mn = range.getStart();
                mx = range.getEnd();

                if (r != nullptr)
                {
                    auto rr = juce::FloatVectorOperations::findMinAndMax (r + i0, i1 - i0);
                    mn = juce::jmin (mn, rr.getStart());
                    mx = juce::jmax (mx, rr.getEnd());
                }
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
        outline.preallocateSpace (6 * columns + 8);
        outline.startNewSubPath (x0, columnTop[0]);

        for (int c = 1; c < columns; ++c)
            outline.lineTo (x0 + (float) c, columnTop[(size_t) c]);

        for (int c = columns - 1; c >= 0; --c)
            outline.lineTo (x0 + (float) c, columnBottom[(size_t) c]);

        outline.closeSubPath();
    }

    void buildOutline (const SampleData& sample, juce::Rectangle<float> area, juce::Path& outline,
                       std::vector<float>& top, std::vector<float>& bottom)
    {
        buildOutline (sample, 0.0, sample.numFrames, (double) sample.numFrames / juce::jmax (1.0, (double) area.getWidth()),
                      area, outline, top, bottom);
    }

    void buildTimelineOutline (const SampleData& sample, int regionStart, int regionEnd, double scaleMs,
                               juce::Rectangle<float> area, juce::Path& outline,
                               std::vector<float>& top, std::vector<float>& bottom)
    {
        buildOutline (sample, (double) regionStart, regionEnd,
                      sample.sampleRate * scaleMs * 0.001 / juce::jmax (1.0, (double) area.getWidth()),
                      area, outline, top, bottom);
    }
}
