#include "SamplerEngine.h"

#include <cmath>

SamplerEngine::SamplerEngine (SampleLibrary& lib, perf::Monitor& pm) : library (lib), perfMon (pm) {}

void SamplerEngine::prepare (double sampleRate) noexcept
{
    hostSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    reset();
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
        if (v.sample != nullptr && library.getForAudio (v.slot) != v.sample)
            stopVoice (v);
}

void SamplerEngine::startVoice (int velocity) noexcept
{
    const int slot = library.getSelectedForAudio();
    const SampleData* s = library.getForAudio (slot);

    if (s == nullptr || s->numFrames <= 0)
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

    target->sample    = s;
    target->slot      = slot;
    target->pos       = 0.0;
    target->unity     = std::abs (inc - 1.0) < 1.0e-9;
    target->increment = target->unity ? 1.0 : inc;
    target->gain      = (float) velocity * (1.0f / 127.0f);
    target->age       = ++ageCounter;
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
    const float gain = v.gain;

    if (v.unity)
    {
        // Same rate as the host: straight SIMD multiply-add, no interpolation.
        const int start = (int) v.pos;
        const int count = juce::jmin (n, s.numFrames - start);

        if (count > 0)
        {
            juce::FloatVectorOperations::addWithMultiply (left,  srcL + start, gain, count);
            juce::FloatVectorOperations::addWithMultiply (right, srcR + start, gain, count);
            v.pos += count;
        }
    }
    else
    {
        // Different rate: linear interpolation at the pitch-preserving ratio.
        // The sample is padded with zeroed frames, so reading idx + 1 is always safe.
        double pos = v.pos;
        const double inc = v.increment;
        const int count = juce::jmin (n, (int) std::ceil (((double) s.numFrames - pos) / inc));

        if (s.numChannels > 1)
        {
            for (int i = 0; i < count; ++i)
            {
                const int idx = (int) pos;
                const float frac = (float) (pos - (double) idx);
                left[i]  += gain * (srcL[idx] + frac * (srcL[idx + 1] - srcL[idx]));
                right[i] += gain * (srcR[idx] + frac * (srcR[idx + 1] - srcR[idx]));
                pos += inc;
            }
        }
        else
        {
            for (int i = 0; i < count; ++i)
            {
                const int idx = (int) pos;
                const float frac = (float) (pos - (double) idx);
                const float x = gain * (srcL[idx] + frac * (srcL[idx + 1] - srcL[idx]));
                left[i]  += x;
                right[i] += x;
                pos += inc;
            }
        }

        v.pos = pos;
    }

    if (v.pos >= (double) s.numFrames)
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
