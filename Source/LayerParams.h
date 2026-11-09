#pragma once

#include <juce_events/juce_events.h>

#include "Envelope.h"
#include "PerfMonitor.h"

// One modulatable parameter of a layer (speed or level): either a knob value or an envelope
// that plays over the length of the sound. Message thread edits, audio thread reads.
class ModParam
{
public:
    enum class Kind { Speed, Level };

    ModParam (Kind, perf::Monitor&);

    Kind getKind() const noexcept         { return kind; }
    // The knob and the envelope axis work in "units": speed as a ratio (x0.25 .. x4, logarithmic) and
    // level in dB (-60 .. 0, linear in dB; the bottom of the range is silence). The audio thread only
    // ever sees the converted value: the speed ratio or a linear gain.
    float getMin() const noexcept         { return kind == Kind::Speed ? 0.25f : -60.0f; }
    float getMax() const noexcept         { return kind == Kind::Speed ? 4.0f : 0.0f; }
    float getDefault() const noexcept     { return kind == Kind::Speed ? 1.0f : 0.0f; }
    float unitsFromNorm (double p) const noexcept;                                          // envelope height 0..1 -> units
    double toNorm (float units) const noexcept;
    float toAudio (float units) const noexcept;                                             // units -> speed ratio / linear gain
    float fromNorm (double p) const noexcept { return toAudio (unitsFromNorm (p)); }        // envelope height -> audio value

    // knob and mode: single atomics, readable from any thread
    float getKnob() const noexcept        { return knob.load (std::memory_order_relaxed); }                // in units
    float getAudioValue() const noexcept  { return audioValue.load (std::memory_order_relaxed); }          // what the engine uses
    void setKnob (float units) noexcept
    {
        units = juce::jlimit (getMin(), getMax(), units);
        knob.store (units, std::memory_order_relaxed);
        audioValue.store (toAudio (units), std::memory_order_relaxed);
    }
    bool isEnvelopeMode() const noexcept  { return envelopeMode.load (std::memory_order_relaxed); }
    void setEnvelopeMode (bool b) noexcept { envelopeMode.store (b, std::memory_order_relaxed); }

    // envelope: edited on the message thread; rebuilds the table and publishes it lock-free
    mod::Envelope getDefaultEnvelope() const;               // flat at x1 (speed) / gate (level)
    mod::Envelope getEnvelope() const;
    void setEnvelope (const mod::Envelope&);

    // audio thread: newest table. Call once per block.
    const mod::EnvTable& readTable() noexcept { return table.read(); }

private:
    Kind kind;
    perf::Monitor& perfMon;
    std::atomic<float> knob { 1.0f }, audioValue { 1.0f };
    std::atomic<bool> envelopeMode { false };
    mutable juce::CriticalSection lock;                 // message-thread side only (never taken by the audio thread)
    mod::Envelope envelope;
    mod::TripleBuffer<mod::EnvTable> table;
};

// Everything one layer exposes for modulation. A layer is one bank of samples; today the plugin
// has a single layer, and each future layer simply owns its own LayerParams.
class LayerParams : public juce::ChangeBroadcaster
{
public:
    explicit LayerParams (perf::Monitor& pm) : speed (ModParam::Kind::Speed, pm), level (ModParam::Kind::Level, pm) {}

    ModParam speed, level;

    void saveTo (juce::XmlElement& parent) const;       // adds a <Layer> child
    void loadFrom (const juce::XmlElement& layer);      // message thread; notifies listeners
};
