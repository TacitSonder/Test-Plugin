#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "Region.h"

#include <atomic>
#include <vector>

// Immutable audio once published. Created on a loader thread, then shared
// (read-only) between the audio thread and the UI. The only mutable parts are
// two atomics the UI uses to talk to the audio thread.
struct SampleData
{
    static constexpr int kPadFrames      = 4;     // zeroed frames after the audio (safe interpolation reads)
    static constexpr int kMaxPeakBuckets = 4096;  // waveform resolution kept for drawing

    juce::String name;                // file name, e.g. "kick.wav"
    juce::String path;                // full path (saved in the project state)
    double sampleRate = 44100.0;      // sample rate of the file
    int numFrames = 0;
    int numChannels = 0;              // 1 or 2 (extra channels are dropped)
    juce::AudioBuffer<float> audio;   // numChannels x (numFrames + kPadFrames)
    std::vector<float> peaks;         // interleaved (min, max) per bucket
    int numPeakBuckets = 0;

    // Part of the file that plays, packed (start | end << 32). Written by the UI,
    // read once per note-on by the audio thread. 0 = "not set" = whole file.
    mutable std::atomic<juce::uint64> region { 0 };

    // Set by the UI when the sample is taken out of the library; the audio thread
    // then drops any voice still playing it (the memory is kept alive until then).
    mutable std::atomic<bool> removed { false };

    void setRegion (int start, int end) const noexcept
    {
        region.store (region::pack (start, end), std::memory_order_release);
    }

    // Always returns a valid region: 0 <= start < end <= numFrames.
    void getRegion (int& start, int& end) const noexcept
    {
        region::unpack (region.load (std::memory_order_acquire), start, end);

        if (start < 0 || end <= start)      // unset (0, 0) or corrupt: play the whole file
        {
            start = 0;
            end = numFrames;
            return;
        }

        end = juce::jmin (end, numFrames);

        if (start >= end)
        {
            start = 0;
            end = numFrames;
        }
    }

    double getDurationSeconds() const noexcept { return sampleRate > 0.0 ? (double) numFrames / sampleRate : 0.0; }

    size_t getMemoryBytes() const noexcept
    {
        return (size_t) audio.getNumChannels() * (size_t) audio.getNumSamples() * sizeof (float)
             + peaks.size() * sizeof (float);
    }
};
