#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "PerfMonitor.h"
#include "SampleLibrary.h"

#include <array>

// The real-time part. Everything here runs on the audio thread: no locks, no
// allocation, no reference counting, no virtual calls.
//
// Behaviour:
//  * Any MIDI note-on starts the currently selected sample from its beginning
//    at its original speed (pitch is not changed by the note number).
//  * Velocity scales the volume.
//  * Note-off is ignored (one-shot: the sample plays to its end).
//  * CC 120 / CC 123 (all sound off / all notes off) stop everything.
//  * 32 voices; when all are busy the oldest voice is restarted.
class SamplerEngine
{
public:
    static constexpr int kMaxVoices = 32;

    SamplerEngine (SampleLibrary&, perf::Monitor&);

    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    void process (juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi) noexcept;

    int getNumActiveVoices() const noexcept { return activeVoices; }

private:
    struct Voice
    {
        const SampleData* sample = nullptr;   // nullptr = free
        double pos = 0.0;                     // read position in source frames
        double increment = 1.0;               // source frames per output frame
        float gain = 1.0f;
        int slot = 0;
        juce::uint64 age = 0;
        bool unity = true;                    // increment == 1: use the SIMD copy path
    };

    void handleMidi (const juce::uint8* data, int numBytes) noexcept;
    void startVoice (int velocity) noexcept;
    void stopVoice (Voice&) noexcept;
    void dropStaleVoices() noexcept;
    void renderSegment (float* left, float* right, int numSamples) noexcept;
    void renderVoice (Voice&, float* left, float* right, int numSamples) noexcept;

    SampleLibrary& library;
    perf::Monitor& perfMon;
    std::array<Voice, kMaxVoices> voices;
    double hostSampleRate = 44100.0;
    juce::uint64 ageCounter = 0;
    int activeVoices = 0;
};
