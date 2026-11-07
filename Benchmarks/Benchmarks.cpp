// Micro-benchmarks for every performance-critical part of the plugin.
//
// Build: cmake -B build -DCMAKE_BUILD_TYPE=Release -DSAMPLER_BUILD_BENCHMARKS=ON
//        cmake --build build --config Release --target SamplerBenchmarks
// Run:   SamplerBenchmarks        -> prints a table, writes benchmark-results.md,
//                                    exit code 1 if any budget is exceeded.
//
// How to read it
//  * Audio cases are measured per 512-frame block at 48 kHz. "% of block" is the
//    DSP load: time used / time available (10.67 ms). 100 % would be a dropout.
//  * Each case runs several repeats; the MEDIAN is reported (robust against noise),
//    plus the MIN (the best the code can do).
//  * Budgets are deliberately generous (shared CI machines are noisy). They exist to
//    catch large regressions; compare the numbers between runs to spot small ones.
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>
#include <juce_graphics/juce_graphics.h>

#include "PerfMonitor.h"
#include "SampleLibrary.h"
#include "SamplerEngine.h"
#include "WaveformGeometry.h"

#include <algorithm>
#include <cstdio>
#include <functional>

namespace
{
    constexpr double kSampleRate = 48000.0;
    constexpr int kBlock = 512;
    constexpr double kBlockBudgetUs = 1.0e6 * kBlock / kSampleRate;   // 10 666.7 us

    struct Row
    {
        juce::String name;
        double medianUs = 0.0, minUs = 0.0;
        double limit = 0.0;          // in the row's unit (see isPercent)
        bool isPercent = true;       // true: limit is % of the block budget; false: microseconds / nanoseconds
        juce::String unit;
        juce::String note;
        bool pass = true;
        double value = 0.0;          // the number compared with `limit`
    };

    std::vector<Row> rows;

    double toUs (juce::int64 ticks) { return 1.0e6 * (double) ticks / (double) juce::Time::getHighResolutionTicksPerSecond(); }

    // Times `iterations` calls of body() per repeat; setup() runs (untimed) before each repeat.
    // Returns {median, min} microseconds per iteration.
    std::pair<double, double> measure (int repeats, int iterations,
                                       const std::function<void()>& setup,
                                       const std::function<void()>& body)
    {
        std::vector<double> samples;
        for (int r = 0; r < repeats + 1; ++r)            // first repeat is a warm-up
        {
            setup();
            const auto t0 = juce::Time::getHighResolutionTicks();
            for (int i = 0; i < iterations; ++i)
                body();
            const double us = toUs (juce::Time::getHighResolutionTicks() - t0) / iterations;
            if (r > 0) samples.push_back (us);
        }
        std::sort (samples.begin(), samples.end());
        return { samples[samples.size() / 2], samples.front() };
    }

    void addBlockRow (const juce::String& name, std::pair<double, double> t, double limitPercent, const juce::String& note = {})
    {
        Row r;
        r.name = name; r.medianUs = t.first; r.minUs = t.second;
        r.isPercent = true; r.limit = limitPercent; r.unit = "us";
        r.value = 100.0 * t.first / kBlockBudgetUs;
        r.pass = r.value <= limitPercent;
        r.note = note;
        rows.push_back (r);
    }

    void addAbsRow (const juce::String& name, std::pair<double, double> t, double limitUs, const juce::String& note = {})
    {
        Row r;
        r.name = name; r.medianUs = t.first; r.minUs = t.second;
        r.isPercent = false; r.limit = limitUs; r.unit = "us";
        r.value = t.first;
        r.pass = r.value <= limitUs;
        r.note = note;
        rows.push_back (r);
    }

    float gen (int i) { return 0.5f * std::sin ((float) i * 0.05f); }

    juce::File writeWav (const juce::File& dir, const juce::String& name, double sr, int ch, int frames)
    {
        auto f = dir.getChildFile (name);
        f.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), sr, (unsigned) ch, 24, {}, 0));
        juce::AudioBuffer<float> b (ch, frames);
        for (int c = 0; c < ch; ++c)
            for (int i = 0; i < frames; ++i)
                b.setSample (c, i, gen (i + c * 7));
        w->writeFromAudioSampleBuffer (b, 0, frames);
        return f;
    }

    bool loadOne (SampleLibrary& lib, const juce::File& f)
    {
        lib.clear();
        lib.addFiles ({ f.getFullPathName() });
        for (int t = 0; t < 20000; t += 2)
        {
            if (lib.getNumLoaded() == 1) return true;
            juce::MessageManager::getInstance()->runDispatchLoopUntil (2);
        }
        return false;
    }

    juce::MidiBuffer noteBurst (int count, int velocity = 100)
    {
        juce::MidiBuffer m;
        for (int i = 0; i < count; ++i)
            m.addEvent (juce::MidiMessage::noteOn (1, 30 + (i % 60), (juce::uint8) velocity), (i * kBlock) / juce::jmax (1, count));
        return m;
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;

    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("sampledrop_bench");
    dir.createDirectory();

    std::printf ("Sample Drop benchmarks  (block %d @ %.0f Hz, budget %.1f us/block, perf monitor %s)\n\n",
                 kBlock, kSampleRate, kBlockBudgetUs, SAMPLER_PERF_ENABLED ? "ON" : "OFF");

    perf::Monitor pm;
    SampleLibrary lib (pm);
    SamplerEngine eng (lib, pm);
    pm.prepare (kSampleRate);

    juce::AudioBuffer<float> buf (2, kBlock);
    const juce::MidiBuffer noMidi;

    // The same work processBlock() does, including the profiler's own overhead.
    auto processBlock = [&] (const juce::MidiBuffer& midi)
    {
        const auto t0 = perf::Monitor::now();
        eng.process (buf, midi);
        pm.recordAudioBlock (perf::Monitor::now() - t0, kBlock, eng.getNumActiveVoices());
    };

    // ---------------------------------------------------------------- audio
    const auto file48 = writeWav (dir, "long48.wav", 48000.0, 2, 48000 * 30);   // matches the host: SIMD path
    const auto file44 = writeWav (dir, "long44.wav", 44100.0, 2, 44100 * 30);   // needs rate conversion
    const int blocks = 1500;                                                       // 16 s of audio per repeat

    auto idle = measure (9, 4000, [&] { eng.prepare (kSampleRate); }, [&] { processBlock (noMidi); });
    addBlockRow ("audio: idle block (no voices)", idle, 1.0, "fixed cost of process() + profiler");

    auto sustained = [&] (const juce::File& f, const char* label, double limit32)
    {
        if (! loadOne (lib, f)) { std::printf ("could not load %s\n", f.getFileName().toRawUTF8()); return; }
        double med1 = 0, med32 = 0;
        for (int voices : { 1, 8, 32 })
        {
            const auto burst = noteBurst (voices);
            auto t = measure (7, blocks,
                              [&] { eng.prepare (kSampleRate); processBlock (burst); },   // start the voices (untimed)
                              [&] { processBlock (noMidi); });
            const double limit = voices == 1 ? 1.0 : voices == 8 ? limit32 / 3.0 : limit32;
            addBlockRow ("audio: " + juce::String (voices) + " voice" + (voices > 1 ? "s" : "") + ", " + label, t, limit);
            if (voices == 1)  med1 = t.first;
            if (voices == 32) med32 = t.first;
        }
        const double perVoice = (med32 - idle.first) / 31.0;
        if (perVoice > 0.0)
            rows.back().note = juce::String (perVoice, 2) + " us per voice per block";
        (void) med1;
    };
    sustained (file48, "same rate (SIMD)", 20.0);
    sustained (file44, "rate conversion", 40.0);

    // region playback: very short regions retriggered by 32 notes every block (worst case: start + fades)
    if (loadOne (lib, file48))
    {
        lib.setRegion (0, 10000, 10300);
        const auto burst = noteBurst (32);
        auto t = measure (9, 2000, [&] { eng.prepare (kSampleRate); }, [&] { processBlock (burst); });
        addBlockRow ("audio: 32 note-ons/block, 300-frame region with fades", t, 40.0, "retrigger storm: voice start + fade ramps");
    }

    {   // MIDI parsing only (nothing loaded -> notes are ignored)
        lib.clear();
        const auto burst = noteBurst (64);
        auto t = measure (9, 4000, [&] { eng.prepare (kSampleRate); }, [&] { processBlock (burst); });
        addBlockRow ("audio: 64 MIDI events/block, no sample", t, 5.0, "parse + block splitting");
    }

    // ------------------------------------------------------------- loading
    {
        const int frames = 48000 * 10;
        const auto f = writeWav (dir, "decode10s.wav", 48000.0, 2, frames);
        const double mb = (double) frames * 2 * 4 / (1024.0 * 1024.0);
        auto t = measure (5, 1, [&] { lib.clear(); }, [&] { loadOne (lib, f); });
        addAbsRow ("load: 10 s stereo 24-bit WAV (end to end)", t, 1.0e6,
                   juce::String (mb / (t.first / 1.0e6), 0) + " MB/s decoded");
    }

    // ------------------------------------------------------------ waveform
    if (loadOne (lib, file48))
    {
        const auto sample = lib.getSample (0);
        juce::Path path; std::vector<float> a, b;
        for (int w : { 600, 1200, 2400, 3840 })
        {
            const juce::Rectangle<float> area (0.0f, 0.0f, (float) w, 300.0f);
            auto t = measure (9, 200, [] {}, [&] { waveform::buildOutline (*sample, area, path, a, b); });
            addAbsRow ("ui: waveform outline, " + juce::String (w) + " px wide (30 s file)", t, 3000.0,
                       w == 3840 ? "runs on every resize step / file switch" : "");
        }
    }

    // ------------------------------------------------------ profiler overhead
    {
        const int N = 20000;
        auto t1 = measure (9, N, [] {}, [&] { perf::ScopedTimer s (pm, perf::Id::Midi); });
        auto t2 = measure (9, N, [] {}, [&] { pm.recordAudioBlock (1000, kBlock, 3); });
        Row r1; r1.name = "profiler: one ScopedTimer"; r1.medianUs = t1.first; r1.minUs = t1.second;
        r1.isPercent = false; r1.limit = 1.0; r1.unit = "us"; r1.value = t1.first; r1.pass = t1.first <= 1.0;
        r1.note = juce::String (t1.first * 1000.0, 0) + " ns per scope";
        rows.push_back (r1);
        Row r2 = r1; r2.name = "profiler: recordAudioBlock"; r2.medianUs = t2.first; r2.minUs = t2.second;
        r2.value = t2.first; r2.pass = t2.first <= 1.0; r2.note = juce::String (t2.first * 1000.0, 0) + " ns per block";
        rows.push_back (r2);
    }

    // --------------------------------------------------------------- report
    juce::String text, md;
    md << "### Sample Drop benchmarks\n\n"
       << "Block " << kBlock << " @ " << (int) kSampleRate << " Hz, budget " << juce::String (kBlockBudgetUs, 1)
       << " us per block. Median of repeated runs; `limit` is the regression budget.\n\n"
       << "| case | median | min | % of block | limit | result | note |\n|---|---:|---:|---:|---:|:---:|---|\n";

    bool allPass = true;
    for (const auto& r : rows)
    {
        allPass = allPass && r.pass;
        const juce::String pct = r.isPercent ? juce::String (100.0 * r.medianUs / kBlockBudgetUs, 2) + " %" : juce::String ("-");
        const juce::String lim = r.isPercent ? "<= " + juce::String (r.limit, 0) + " %" : "<= " + juce::String (r.limit, 0) + " us";

        text << r.name.paddedRight (' ', 56) << juce::String (r.medianUs, 2).paddedLeft (' ', 11) << " us"
             << juce::String (r.minUs, 2).paddedLeft (' ', 11) << " us  " << pct.paddedLeft (' ', 9)
             << "  " << (r.pass ? "ok  " : "SLOW") << "  " << r.note << "\n";

        md << "| " << r.name << " | " << juce::String (r.medianUs, 2) << " us | " << juce::String (r.minUs, 2)
           << " us | " << pct << " | " << lim << " | " << (r.pass ? "ok" : "**SLOW**") << " | " << r.note << " |\n";
    }

    std::printf ("%s%-56s %13s %13s %10s\n", "", "case", "median", "min", "% of block");
    std::printf ("%s\n%s\n", juce::String::repeatedString ("-", 110).toRawUTF8(), text.toRawUTF8());
    std::printf ("%s\n", allPass ? "ALL BENCHMARKS WITHIN BUDGET" : "SOME BENCHMARKS EXCEEDED THEIR BUDGET");

    juce::File ("benchmark-results.md").replaceWithText (md);
    return allPass ? 0 : 1;
}
