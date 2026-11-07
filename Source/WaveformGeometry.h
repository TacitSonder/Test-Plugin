#pragma once

#include <juce_graphics/juce_graphics.h>

#include "SampleData.h"

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
}
