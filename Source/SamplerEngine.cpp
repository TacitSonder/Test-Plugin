#include "SamplerEngine.h"

#include <cmath>

namespace
{
    // Scalar renderer with linear interpolation and a per-sample gain ramp.
    // Used for rate conversion and for the (very short) fade-in / fade-out parts.
    // The source is padded with zero frames, so reading idx + 1 is always safe.
    template <bool Stereo>
    double renderScalar (const float* srcL, const float* srcR, double pos, double inc, int count,
                         float gain, float dGain, float* outL, float* outR) noexcept
    {
        for (int i = 0; i < count; ++i)
        {
            const int idx = (int) pos;
            const float frac = (float) (pos - (double) idx);
            const float l = srcL[idx] + frac * (srcL[idx + 1] - srcL[idx]);

            if constexpr (Stereo)
            {
                const float r = srcR[idx] + frac * (srcR[idx + 1] - srcR[idx]);
                outL[i] += gain * l;
                outR[i] += gain * r;
            }
            else
            {
                const float x = gain * l;
                outL[i] += x;
                outR[i] += x;
            }

            gain += dGain;
            pos += inc;
        }

        return pos;
    }
}

SamplerEngine::SamplerEngine (SampleLibrary& lib, LayerParams& lp, perf::Monitor& pm) : library (lib), params (lp), perfMon (pm) {}

void SamplerEngine::prepare (double sampleRate) noexcept
{
    hostSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    reset();
    levelPrev = params.level.getAudioValue();                                  // no ramp on the first block
    lastTriggerCount = triggerCount.load (std::memory_order_acquire);   // ignore clicks from before
}

void SamplerEngine::reset() noexcept
{
    for (auto& v : voices)
        v.sample = nullptr;
    activeVoices = 0;
}

void SamplerEngine::stopVoice (Voice& v) noexcept
{
    if (v.sample != nullptr)
    {
        v.sample = nullptr;
        --activeVoices;
    }
}

// A sample that was removed from the library must never be read again.
void SamplerEngine::dropStaleVoices() noexcept
{
    if (activeVoices == 0)
        return;

    for (auto& v : voices)
        if (v.sample != nullptr && v.sample->removed.load (std::memory_order_acquire))
            stopVoice (v);
}

void SamplerEngine::startVoice (int velocity) noexcept
{
    const SampleData* s = library.getForAudio (library.getSelectedForAudio());

    if (s == nullptr || s->numFrames <= 0)
        return;

    int start = 0, end = 0;
    s->getRegion (start, end);

    if (end - start < 1)
        return;

    Voice* target = nullptr;

    for (auto& v : voices)
        if (v.sample == nullptr) { target = &v; break; }

    if (target == nullptr)
    {
        target = &voices[0];                       // steal the oldest voice
        for (auto& v : voices)
            if (v.age < target->age)
                target = &v;
    }
    else
    {
        ++activeVoices;
    }

    const double baseInc = s->sampleRate / hostSampleRate;
    const double step = baseInc * bm.speedAt (0.0);                      // fade length follows the starting speed

    // Fade only where the region cuts into the audio (not at the true file start/end).
    const bool fadeIn  = start > 0;
    const bool fadeOut = end < s->numFrames;
    const double fadeSrc = (fadeIn || fadeOut)
                               ? juce::jmin ((double) kFadeFrames * step, 0.5 * (double) (end - start))
                               : 0.0;

    target->sample       = s;
    target->pos          = (double) start;
    target->startPos     = (double) start;
    target->end          = (double) end;
    target->invRegion    = 1.0 / (double) (end - start);
    target->fadeInEnd    = fadeIn  ? (double) start + fadeSrc : (double) start;
    target->fadeOutStart = fadeOut ? (double) end - fadeSrc   : (double) end;
    target->invFade      = fadeSrc > 0.0 ? 1.0 / fadeSrc : 0.0;
    target->baseInc      = baseInc;
    target->gain         = (float) velocity * (1.0f / 127.0f);
    target->delay        = (int) std::lround ((double) s->delayMs.load (std::memory_order_relaxed) * 0.001 * hostSampleRate);
    target->age          = ++ageCounter;
}

// Raw MIDI bytes are parsed directly: no MidiMessage objects, no allocation.
void SamplerEngine::handleMidi (const juce::uint8* data, int numBytes) noexcept
{
    if (numBytes < 3)
        return;

    const int type = data[0] & 0xF0;

    if (type == 0x90 && data[2] > 0)
    {
        startVoice (data[2]);
    }
    else if (type == 0xB0 && (data[1] == 120 || data[1] == 123))
    {
        for (auto& v : voices)
            stopVoice (v);
    }
}

void SamplerEngine::renderVoice (Voice& v, float* left, float* right, int n) noexcept
{
    const SampleData& s = *v.sample;
    const float* srcL = s.audio.getReadPointer (0);
    const float* srcR = s.numChannels > 1 ? s.audio.getReadPointer (1) : srcL;
    const bool stereo = s.numChannels > 1;
    int done = 0;                                     // frames of this segment already handled

    if (v.delay > 0)                                  // start delay: stay silent first
    {
        const int skip = juce::jmin (n, v.delay);
        v.delay -= skip;
        left += skip;
        right += skip;
        n -= skip;
        done = skip;
    }

    // With an envelope active the voice is rendered in short chunks so speed and level can follow
    // it; otherwise a chunk is a whole phase (fade-in, body, fade-out) exactly as without modulation.
    const int maxChunk = (bm.speedTable != nullptr || bm.levelTable != nullptr) ? kModChunk : 1 << 30;

    while (n > 0)
    {
        if (v.pos >= v.end - 1.0e-9)
        {
            stopVoice (v);
            return;
        }

        const double x0 = (v.pos - v.startPos) * v.invRegion;                       // where we are in the sound, 0..1
        const double inc = v.baseInc * juce::jmax (0.01, bm.speedAt (x0));
        const float level0 = bm.levelTable != nullptr ? bm.levelTable->at (x0)
                                                      : bm.levelStart + bm.levelSlope * (float) done;

        double boundary;
        float fade0 = 1.0f, dFade = 0.0f;                                           // edge fade: value now, change per frame

        if (v.pos < v.fadeInEnd)
        {
            boundary = v.fadeInEnd;
            fade0 = (float) ((v.pos - v.startPos) * v.invFade);
            dFade = (float) (inc * v.invFade);
        }
        else if (v.pos < v.fadeOutStart)
        {
            boundary = v.fadeOutStart;
        }
        else
        {
            boundary = v.end;
            fade0 = (float) ((v.end - v.pos) * v.invFade);
            dFade = -(float) (inc * v.invFade);
        }

        // pos < boundary here, so count >= 1.
        const int count = juce::jmin (n, maxChunk, (int) std::ceil ((boundary - v.pos) / inc));

        float levelSlope = bm.levelSlope;
        if (bm.levelTable != nullptr)
            levelSlope = (bm.levelTable->at (juce::jmin (1.0, x0 + (double) count * inc * v.invRegion)) - level0) / (float) count;

        const float gain  = v.gain * level0 * fade0;
        const float dGain = v.gain * (level0 * dFade + fade0 * levelSlope);

        if (std::abs (inc - 1.0) < 1.0e-9 && std::abs (dGain) < 1.0e-9f && std::abs (v.pos - std::floor (v.pos)) < 1.0e-9)
        {
            // Same rate, constant gain, whole-frame position: straight SIMD multiply-add.
            const int start = (int) v.pos;
            juce::FloatVectorOperations::addWithMultiply (left,  srcL + start, gain, count);
            juce::FloatVectorOperations::addWithMultiply (right, srcR + start, gain, count);
            v.pos += count;
        }
        else if (stereo)
        {
            v.pos = renderScalar<true> (srcL, srcR, v.pos, inc, count, gain, dGain, left, right);
        }
        else
        {
            v.pos = renderScalar<false> (srcL, srcR, v.pos, inc, count, gain, dGain, left, right);
        }

        left += count;
        right += count;
        n -= count;
        done += count;
    }

    if (v.pos >= v.end - 1.0e-9)
        stopVoice (v);
}

void SamplerEngine::renderSegment (float* left, float* right, int numSamples, int offsetInBlock) noexcept
{
    bm.levelStart = levelPrev + bm.levelSlope * (float) offsetInBlock;

    for (auto& v : voices)
        if (v.sample != nullptr)
            renderVoice (v, left, right, numSamples);
}

void SamplerEngine::process (juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi) noexcept
{
    library.beginAudioBlock();        // must be first: lets the library know a new block started

    const int numSamples = buffer.getNumSamples();
    buffer.clear();

    if (buffer.getNumChannels() < 2 || numSamples <= 0)
        return;

    dropStaleVoices();

    // The layer's parameters, read once for the whole block.
    bm.speedKnob  = (double) params.speed.getAudioValue();
    bm.speedTable = params.speed.isEnvelopeMode() ? &params.speed.readTable() : nullptr;
    bm.levelTable = params.level.isEnvelopeMode() ? &params.level.readTable() : nullptr;
    const float levelNow = params.level.getAudioValue();
    bm.levelSlope = (levelNow - levelPrev) / (float) numSamples;      // knob moves are smoothed across the block

    float* const left  = buffer.getWritePointer (0);
    float* const right = buffer.getWritePointer (1);

    // "Play" button clicks from the GUI: start at the top of this block.
    const auto clicks = triggerCount.load (std::memory_order_acquire);

    if (clicks != lastTriggerCount)
    {
        const int toStart = (int) juce::jmin ((juce::uint32) 4, clicks - lastTriggerCount);
        lastTriggerCount = clicks;
        perfMon.record (perf::Id::TriggerLatency, perf::Monitor::now() - triggerTicks.load (std::memory_order_relaxed));

        for (int i = 0; i < toStart; ++i)
            startVoice (kGuiVelocity);
    }

    juce::int64 midiTicks = 0, renderTicks = 0;
    bool hadMidi = false, rendered = false;
    int pos = 0;

    // Split the block at each MIDI event so notes start sample-accurately.
    for (const auto meta : midi)
    {
        const int eventPos = juce::jlimit (pos, numSamples, meta.samplePosition);

        if (eventPos > pos && activeVoices > 0)
        {
            const auto t = perf::Monitor::now();
            renderSegment (left + pos, right + pos, eventPos - pos, pos);
            renderTicks += perf::Monitor::now() - t;
            rendered = true;
        }
        pos = eventPos;

        const auto t = perf::Monitor::now();
        handleMidi (meta.data, meta.numBytes);
        midiTicks += perf::Monitor::now() - t;
        hadMidi = true;
    }

    if (pos < numSamples && activeVoices > 0)
    {
        const auto t = perf::Monitor::now();
        renderSegment (left + pos, right + pos, numSamples - pos, pos);
        renderTicks += perf::Monitor::now() - t;
        rendered = true;
    }

    levelPrev = levelNow;

    if (hadMidi)  perfMon.record (perf::Id::Midi, midiTicks);
    if (rendered) perfMon.record (perf::Id::VoiceRender, renderTicks);
}
