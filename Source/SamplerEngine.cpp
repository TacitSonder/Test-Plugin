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

SamplerEngine::SamplerEngine (SampleLibrary& lib, perf::Monitor& pm) : library (lib), perfMon (pm) {}

void SamplerEngine::prepare (double sampleRate) noexcept
{
    hostSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    reset();
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

    const double inc = s->sampleRate / hostSampleRate;
    const bool unity = std::abs (inc - 1.0) < 1.0e-9;
    const double step = unity ? 1.0 : inc;

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
    target->fadeInEnd    = fadeIn  ? (double) start + fadeSrc : (double) start;
    target->fadeOutStart = fadeOut ? (double) end - fadeSrc   : (double) end;
    target->invFade      = fadeSrc > 0.0 ? 1.0 / fadeSrc : 0.0;
    target->unity        = unity;
    target->increment    = step;
    target->gain         = (float) velocity * (1.0f / 127.0f);
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

    // A voice has up to three phases: fade-in, body, fade-out. The body is the
    // hot path (SIMD when the rates match); the fades are at most kFadeFrames long.
    while (n > 0)
    {
        if (v.pos >= v.end - 1.0e-9)
        {
            stopVoice (v);
            return;
        }

        double boundary;
        float gain, dGain;
        bool ramp;

        if (v.pos < v.fadeInEnd)
        {
            boundary = v.fadeInEnd;
            gain  = v.gain * (float) ((v.pos - v.startPos) * v.invFade);
            dGain = v.gain * (float) (v.increment * v.invFade);
            ramp = true;
        }
        else if (v.pos < v.fadeOutStart)
        {
            boundary = v.fadeOutStart;
            gain = v.gain;
            dGain = 0.0f;
            ramp = false;
        }
        else
        {
            boundary = v.end;
            gain  = v.gain * (float) ((v.end - v.pos) * v.invFade);
            dGain = -v.gain * (float) (v.increment * v.invFade);
            ramp = true;
        }

        // pos < boundary here, so count >= 1.
        const int count = juce::jmin (n, (int) std::ceil ((boundary - v.pos) / v.increment));

        if (v.unity && ! ramp)
        {
            const int start = (int) v.pos;
            juce::FloatVectorOperations::addWithMultiply (left,  srcL + start, gain, count);
            juce::FloatVectorOperations::addWithMultiply (right, srcR + start, gain, count);
            v.pos += count;
        }
        else if (stereo)
        {
            v.pos = renderScalar<true> (srcL, srcR, v.pos, v.increment, count, gain, dGain, left, right);
        }
        else
        {
            v.pos = renderScalar<false> (srcL, srcR, v.pos, v.increment, count, gain, dGain, left, right);
        }

        left += count;
        right += count;
        n -= count;
    }

    if (v.pos >= v.end - 1.0e-9)
        stopVoice (v);
}

void SamplerEngine::renderSegment (float* left, float* right, int numSamples) noexcept
{
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
            renderSegment (left + pos, right + pos, eventPos - pos);
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
        renderSegment (left + pos, right + pos, numSamples - pos);
        renderTicks += perf::Monitor::now() - t;
        rendered = true;
    }

    if (hadMidi)  perfMon.record (perf::Id::Midi, midiTicks);
    if (rendered) perfMon.record (perf::Id::VoiceRender, renderTicks);
}
