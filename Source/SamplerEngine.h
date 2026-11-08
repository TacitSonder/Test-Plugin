#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "LayerParams.h"
#include "PerfMonitor.h"
#include "SampleLibrary.h"

#include <array>
#include <atomic>

// The real-time part. Everything here runs on the audio thread: no locks, no
// allocation, no reference counting, no virtual calls.
//
// Behaviour:
//  * Any MIDI note-on starts the currently selected file at its original speed
//    (the note number does not change the pitch).
//  * Only the selected region of the file plays. If the region starts or ends
//    inside the file, a very short fade (kFadeFrames) hides the cut.
//  * The layer's SPEED and LEVEL are applied live: each is a knob value or an envelope over the sound.
//  * Velocity scales the volume.
//  * Note-off is ignored: one-shot, the region plays to its end.
//  * CC 120 / CC 123 (all sound off / all notes off) stop everything.
//  * 32 voices; when all are busy the oldest voice is restarted.
//  * The GUI "Play" button calls requestTrigger(): same as a note-on of kGuiNote.
class SamplerEngine
{
public:
    static constexpr int kMaxVoices  = 32;
    static constexpr int kFadeFrames = 64;     // ~1.3 ms at 48 kHz
    static constexpr int kGuiNote    = 60;     // middle C
    static constexpr int kGuiVelocity = 100;
    static constexpr int kModChunk   = 32;     // frames between envelope look-ups while an envelope is active

    SamplerEngine (SampleLibrary&, LayerParams&, perf::Monitor&);

    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    void process (juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi) noexcept;

    // Any thread, lock-free. The note starts at the beginning of the next audio block.
    void requestTrigger() noexcept
    {
        triggerTicks.store (perf::Monitor::now(), std::memory_order_relaxed);
        triggerCount.fetch_add (1, std::memory_order_release);
    }

    int getNumActiveVoices() const noexcept { return activeVoices; }

private:
    struct Voice
    {
        const SampleData* sample = nullptr;   // nullptr = free
        double pos = 0.0;                     // read position, in source frames
        double end = 0.0;                     // end of the region (exclusive)
        double startPos = 0.0;                // start of the region
        double invRegion = 0.0;               // 1 / region length: position -> 0..1 for the envelopes
        double fadeInEnd = 0.0;               // fade-in runs startPos .. fadeInEnd (== startPos: none)
        double fadeOutStart = 0.0;            // fade-out runs fadeOutStart .. end   (== end: none)
        double invFade = 0.0;                 // 1 / fade length in source frames
        double baseInc = 1.0;                 // file rate / host rate; multiplied by the speed
        float gain = 1.0f;                    // velocity
        int delay = 0;                        // output frames still to wait before the sound starts
        juce::uint64 age = 0;
    };

    // Layer parameters as read at the start of the current block.
    struct BlockMod
    {
        double speedKnob = 1.0;
        const mod::EnvTable* speedTable = nullptr;   // non-null = envelope mode
        const mod::EnvTable* levelTable = nullptr;
        float levelStart = 1.0f;                     // knob level at the first frame of the current segment
        float levelSlope = 0.0f;                     // knob level change per output frame (smooths knob moves)

        double speedAt (double x) const noexcept { return speedTable != nullptr ? (double) speedTable->at (x) : speedKnob; }
    };

    void handleMidi (const juce::uint8* data, int numBytes) noexcept;
    void startVoice (int velocity) noexcept;
    void stopVoice (Voice&) noexcept;
    void dropStaleVoices() noexcept;
    void renderSegment (float* left, float* right, int numSamples, int offsetInBlock) noexcept;
    void renderVoice (Voice&, float* left, float* right, int numSamples) noexcept;

    SampleLibrary& library;
    LayerParams& params;
    perf::Monitor& perfMon;
    BlockMod bm;
    float levelPrev = 1.0f;
    std::array<Voice, kMaxVoices> voices;
    double hostSampleRate = 44100.0;
    juce::uint64 ageCounter = 0;
    int activeVoices = 0;

    std::atomic<juce::uint32> triggerCount { 0 };
    std::atomic<juce::int64> triggerTicks { 0 };
    juce::uint32 lastTriggerCount = 0;
};
