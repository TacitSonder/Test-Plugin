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
    float getMin() const noexcept         { return kind == Kind::Speed ? 0.25f : 0.0f; }     // speed: x0.25 .. x4
    float getMax() const noexcept         { return kind == Kind::Speed ? 4.0f : 1.0f; }     // level: 0 .. 1
    float getDefault() const noexcept     { return 1.0f; }
    float fromNorm (double p) const noexcept;                                               // envelope height 0..1 -> value
    double toNorm (float value) const noexcept;

    // knob and mode: single atomics, readable from any thread
    float getKnob() const noexcept        { return knob.load (std::memory_order_relaxed); }
    void setKnob (float v) noexcept       { knob.store (juce::jlimit (getMin(), getMax(), v), std::memory_order_relaxed); }
    bool isEnvelopeMode() const noexcept  { return envelopeMode.load (std::memory_order_relaxed); }
    void setEnvelopeMode (bool b) noexcept { envelopeMode.store (b, std::memory_order_relaxed); }

    // envelope: edited on the message thread; rebuilds the table and publishes it lock-free
    mod::Envelope getEnvelope() const;
    void setEnvelope (const mod::Envelope&);

    // audio thread: newest table. Call once per block.
    const mod::EnvTable& readTable() noexcept { return table.read(); }

private:
    Kind kind;
    perf::Monitor& perfMon;
    std::atomic<float> knob { 1.0f };
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
