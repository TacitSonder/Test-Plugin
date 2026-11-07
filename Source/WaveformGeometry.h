#pragma once

#include <juce_graphics/juce_graphics.h>

#include "SampleData.h"

#include <cmath>
#include <vector>

namespace waveform
{
    // Builds the filled outline of the waveform for a pixel area, from the
    // precomputed peaks: O(area width), independent of the file length.
    // `scratchTop` / `scratchBottom` are reused between calls to avoid allocation.
    void buildOutline (const SampleData& sample,
                       juce::Rectangle<float> area,
                       juce::Path& outline,
                       std::vector<float>& scratchTop,
                       std::vector<float>& scratchBottom);

    // The part of the file that plays, drawn on a time axis: x = 0 is the trigger moment and
    // `area.getWidth()` pixels span `scaleMs`. Min/max are read from the audio itself (SIMD),
    // so detail is exact at any zoom; cost is proportional to the *visible* frames only.
    // The outline starts at area.getX() (no delay applied: the caller shifts it).
    void buildTimelineOutline (const SampleData& sample, int regionStart, int regionEnd, double scaleMs,
                               juce::Rectangle<float> area, juce::Path& outline,
                               std::vector<float>& scratchTop, std::vector<float>& scratchBottom);

    // Smallest "nice" ruler step (1, 2, 5 x 10^n ms) that keeps ticks >= minSpacingPx apart.
    inline double niceTickStepMs (double scaleMs, double widthPx, double minSpacingPx) noexcept
    {
        const double minStep = scaleMs * minSpacingPx / juce::jmax (1.0, widthPx);
        const double p = std::pow (10.0, std::floor (std::log10 (minStep)));

        for (const double m : { 1.0, 2.0, 5.0 })
            if (m * p >= minStep)
                return m * p;

        return 10.0 * p;
    }
}
