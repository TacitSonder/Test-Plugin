#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>

// Immutable once published. Created on a loader thread, then shared (read-only)
// between the audio thread and the UI.
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

    double getDurationSeconds() const noexcept { return sampleRate > 0.0 ? (double) numFrames / sampleRate : 0.0; }

    size_t getMemoryBytes() const noexcept
    {
        return (size_t) audio.getNumChannels() * (size_t) audio.getNumSamples() * sizeof (float)
             + peaks.size() * sizeof (float);
    }
};
