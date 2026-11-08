#pragma once

#include <juce_graphics/juce_graphics.h>

#include "SampleData.h"

#include <cmath>
#include <vector>

namespace waveform
{
    // Core: filled min/max outline of the audio from `firstFrame` to `endFrame`, one pixel column
    // per `framesPerColumn` frames, starting at area.getX(). Zoomed out (>= 2 peak buckets per
    // column) it reads the precomputed peaks: O(columns + visible/256). Zoomed in it reads the
    // audio itself with SIMD: O(visible frames), at most ~512 x width. Either way the cost never
    // depends on the file length. `scratch*` are reused between calls (no allocation).
    void buildOutline (const SampleData& sample, double firstFrame, int endFrame, double framesPerColumn,
                       juce::Rectangle<float> area, juce::Path& outline,
                       std::vector<float>& scratchTop, std::vector<float>& scratchBottom);

    // Whole file stretched over the area.
    void buildOutline (const SampleData& sample, juce::Rectangle<float> area, juce::Path& outline,
                       std::vector<float>& scratchTop, std::vector<float>& scratchBottom);

    // The part of the file that plays, on a time axis: x = 0 is the trigger moment and the area's
    // width spans `scaleMs`. No delay applied (the caller shifts the outline).
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
