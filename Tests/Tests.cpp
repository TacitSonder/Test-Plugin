// Automated tests for the engine, loader, regions, removal, state and profiler.
// Build: cmake -B build -DSAMPLER_BUILD_TESTS=ON ; cmake --build build --target SamplerTests
// Run:   SamplerTests          (exit code 0 = all passed)
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include "PerfMonitor.h"
#include "PluginProcessor.h"
#include "Region.h"
#include "SampleLibrary.h"
#include "SamplerEngine.h"
#include "WaveformGeometry.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <thread>

static int failures = 0, checks = 0;
#define CHECK(cond, msg) do { ++checks; if (! (cond)) { ++failures; std::printf ("  FAIL: %s  [line %d]\n", msg, __LINE__); } } while (0)

static void pump (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

template <typename Pred>
static bool pumpUntil (Pred p, int timeoutMs = 8000)
{
    for (int t = 0; t < timeoutMs; t += 10) { if (p()) return true; pump (10); }
    return p();
}

static float genL (int i) { return 0.5f * std::sin ((float) i * 0.05f); }
static float genR (int i) { return 0.25f * std::cos ((float) i * 0.03f); }

static juce::File writeWav (const juce::File& dir, const juce::String& name, double sr, int ch, int frames)
{
    auto f = dir.getChildFile (name);
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), sr, (unsigned) ch, 32, {}, 0));
    juce::AudioBuffer<float> b (ch, frames);
    for (int i = 0; i < frames; ++i) { b.setSample (0, i, genL (i)); if (ch > 1) b.setSample (1, i, genR (i)); }
    w->writeFromAudioSampleBuffer (b, 0, frames);
    return f;
}

static juce::MidiMessage noteOn (int note, int vel) { return juce::MidiMessage::noteOn (1, note, (juce::uint8) vel); }

struct Out { std::vector<float> L, R; };

// Runs the engine for `total` frames in blocks; `midiFor(blockIndex, midi)` fills each block's MIDI.
static Out run (SamplerEngine& eng, int total, int block, const std::function<void (int, juce::MidiBuffer&)>& midiFor = {})
{
    Out o;
    juce::AudioBuffer<float> buf (2, block);
    for (int b = 0; b * block < total; ++b)
    {
        juce::MidiBuffer midi;
        if (midiFor) midiFor (b, midi);
        eng.process (buf, midi);
        o.L.insert (o.L.end(), buf.getReadPointer (0), buf.getReadPointer (0) + block);
        o.R.insert (o.R.end(), buf.getReadPointer (1), buf.getReadPointer (1) + block);
    }
    return o;
}

static bool loadAll (SampleLibrary& lib, const juce::StringArray& files)
{
    lib.addFiles (files);
    return pumpUntil ([&] { return lib.getNumLoaded() == files.size(); });
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("sampledrop_tests");
    dir.createDirectory();

    const auto f1 = writeWav (dir, "a_stereo44.wav", 44100.0, 2, 1000);
    const auto f2 = writeWav (dir, "b_mono48.wav",   48000.0, 1, 2000);
    const auto f3 = writeWav (dir, "c_stereo48.wav", 48000.0, 2, 5000);
    auto txt = dir.getChildFile ("notes.txt"); txt.replaceWithText ("hello");
    const auto p1 = f1.getFullPathName(), p2 = f2.getFullPathName(), p3 = f3.getFullPathName();

    // ------------------------------------------------------------ region helpers
    std::printf ("[region] helpers\n");
    {
        int s, e;
        region::unpack (region::pack (123, 456789), s, e);
        CHECK (s == 123 && e == 456789, "pack/unpack");
        s = 500; e = 100; region::sanitize (s, e, 1000);
        CHECK (s == 100 && e == 500, "sanitize swaps");
        s = 10; e = 12; region::sanitize (s, e, 1000);
        CHECK (e - s == region::kMinFrames, "sanitize enforces minimum length");
        s = 990; e = 995; region::sanitize (s, e, 1000);
        CHECK (e == 1000 && e - s == region::kMinFrames, "sanitize keeps min length at the end");
        s = -50; e = 5000; region::sanitize (s, e, 1000);
        CHECK (s == 0 && e == 1000, "sanitize clamps");
        CHECK (region::xToFrame (150.0f, 1000, 100.0f, 200.0f) == 250, "xToFrame");
        CHECK (region::xToFrame (-10.0f, 1000, 100.0f, 200.0f) == 0 && region::xToFrame (999.0f, 1000, 100.0f, 200.0f) == 1000, "xToFrame clamps");
        CHECK (region::hitEdge (101.0f, 100.0f, 300.0f, 7.0f) == region::Edge::Start, "hit start");
        CHECK (region::hitEdge (298.0f, 100.0f, 300.0f, 7.0f) == region::Edge::End, "hit end");
        CHECK (region::hitEdge (200.0f, 100.0f, 300.0f, 7.0f) == region::Edge::None, "no hit in the middle");
        CHECK (region::hitEdge (200.0f, 198.0f, 202.0f, 7.0f) != region::Edge::None, "nearer edge wins when close");
        region::shift (100, 300, 50, 1000, s, e);
        CHECK (s == 150 && e == 350, "shift moves the region");
        region::shift (100, 300, 5000, 1000, s, e);
        CHECK (s == 800 && e == 1000, "shift stops at the file end, keeping the length");
        region::shift (100, 300, -5000, 1000, s, e);
        CHECK (s == 0 && e == 200, "shift stops at the file start, keeping the length");

        SampleData d; d.numFrames = 1000;
        d.getRegion (s, e);
        CHECK (s == 0 && e == 1000, "unset region = whole file");
        d.setRegion (200, 300); d.getRegion (s, e);
        CHECK (s == 200 && e == 300, "region round trip");
        d.setRegion (900, 5000); d.getRegion (s, e);
        CHECK (s == 900 && e == 1000, "region end clamped to file");
    }

    // ------------------------------------------------------------------ library
    std::printf ("[library] loading, ordering, limits\n");
    {
        perf::Monitor pm;
        SampleLibrary lib (pm);

        CHECK (lib.isSupportedFile (f1), "wav supported");
        CHECK (! lib.isSupportedFile (txt), "txt not supported");
        CHECK (lib.addFiles ({ p1, p2, p3, txt.getFullPathName() }) == 3, "3 queued");
        CHECK (pumpUntil ([&] { return lib.getNumLoaded() == 3; }), "3 loaded");
        CHECK (lib.getSample (0)->name == "a_stereo44.wav" && lib.getSample (1)->name == "b_mono48.wav"
               && lib.getSample (2)->name == "c_stereo48.wav", "drop order preserved");
        CHECK (lib.getSample (0)->numChannels == 2 && lib.getSample (1)->numChannels == 1, "channel counts");
        CHECK (lib.getSample (0)->numPeakBuckets == 4, "one peak pair per 256 frames (1000 frames -> 4)");
        CHECK (lib.getStatusMessage().contains ("skipped"), "unsupported file reported");

        int s, e; lib.getSample (2)->getRegion (s, e);
        CHECK (s == 0 && e == 5000, "new file plays whole file by default");

        juce::StringArray many;
        for (int i = 0; i < 9; ++i) many.add (p1);
        CHECK (lib.addFiles (many) == 5, "only 5 more accepted (limit 8)");
        CHECK (lib.getStatusMessage().contains ("ignored"), "overflow reported");
        CHECK (pumpUntil ([&] { return lib.getNumLoaded() == 8; }), "8 loaded");

        lib.setSelected (5);
        CHECK (lib.getSelected() == 5, "select 5");
        lib.clear();
        CHECK (lib.getNumLoaded() == 0 && lib.getSelected() == 0, "cleared");
    }

    std::printf ("[library] remove a single file\n");
    {
        perf::Monitor pm;
        SampleLibrary lib (pm);
        CHECK (loadAll (lib, { p1, p2, p3 }), "loaded 3");

        lib.setSelected (2);                       // c_stereo48
        lib.removeSample (0);                      // remove a_stereo44
        CHECK (lib.getNumLoaded() == 2, "2 left");
        CHECK (lib.getSample (0)->name == "b_mono48.wav" && lib.getSample (1)->name == "c_stereo48.wav", "later files moved up");
        CHECK (lib.getSelected() == 1 && lib.getSample (lib.getSelected())->name == "c_stereo48.wav", "same file stays selected");
        CHECK (lib.getForAudio (0) == lib.getSample (0).get() && lib.getForAudio (1) == lib.getSample (1).get(), "audio slots follow");
        CHECK (lib.getForAudio (2) == nullptr, "freed slot unpublished");

        lib.removeSample (1);                      // remove the selected file
        CHECK (lib.getNumLoaded() == 1 && lib.getSelected() == 0, "selection moves to a valid file");
        lib.removeSample (7);                      // out of range: ignored
        CHECK (lib.getNumLoaded() == 1, "bad index ignored");
        lib.removeSample (0);
        CHECK (lib.getNumLoaded() == 0, "all removed one by one");

        CHECK (loadAll (lib, { p1 }), "can load again after removals");
    }

    // ------------------------------------------------------------------- engine
    std::printf ("[engine] unity-rate whole-file playback is sample exact\n");
    {
        perf::Monitor pm; SampleLibrary lib (pm); LayerParams lp (pm); SamplerEngine eng (lib, lp, pm);
        CHECK (loadAll (lib, { p1, p2, p3 }), "loaded");
        pm.prepare (48000.0); eng.prepare (48000.0);
        lib.setSelected (2);

        auto o = run (eng, 6144, 512, [] (int b, juce::MidiBuffer& m) { if (b == 0) m.addEvent (noteOn (60, 100), 100); });
        const float g = 100.0f / 127.0f;
        double maxErr = 0.0; bool silentBefore = true, silentAfter = true;
        for (int i = 0; i < 100; ++i) silentBefore &= (o.L[(size_t) i] == 0.0f);
        for (int k = 0; k < 5000; ++k)
        {
            maxErr = juce::jmax (maxErr, (double) std::abs (o.L[(size_t) (100 + k)] - g * genL (k)));
            maxErr = juce::jmax (maxErr, (double) std::abs (o.R[(size_t) (100 + k)] - g * genR (k)));
        }
        for (int i = 5100; i < 6144; ++i) silentAfter &= (o.L[(size_t) i] == 0.0f && o.R[(size_t) i] == 0.0f);
        CHECK (silentBefore, "silence before the note (sample-accurate start)");
        CHECK (maxErr < 1.0e-5, "output equals file * velocity gain");
        CHECK (silentAfter && eng.getNumActiveVoices() == 0, "silence and voice freed after the end");
    }

    std::printf ("[engine] playing only a selected region (with fades)\n");
    {
        perf::Monitor pm; SampleLibrary lib (pm); LayerParams lp (pm); SamplerEngine eng (lib, lp, pm);
        CHECK (loadAll (lib, { p3 }), "loaded");
        pm.prepare (48000.0); eng.prepare (48000.0);
        const float g = 127.0f / 127.0f;
        const int F = SamplerEngine::kFadeFrames;

        auto check = [&] (int start, int end, const char* what)
        {
            lib.setRegion (0, start, end);
            eng.reset();
            auto o = run (eng, 6144, 512, [] (int b, juce::MidiBuffer& m) { if (b == 0) m.addEvent (noteOn (60, 127), 0); });

            const int len = end - start;
            const double fade = juce::jmin ((double) F, 0.5 * len);
            double maxErr = 0.0;
            for (int k = 0; k < len; ++k)
            {
                const int pos = start + k;
                double gain = g;
                if (start > 0 && k < fade)               gain *= (double) k / fade;
                if (end < 5000 && (len - k) <= fade)     gain *= (double) (len - k) / fade;
                maxErr = juce::jmax (maxErr, std::abs ((double) o.L[(size_t) k] - gain * genL (pos)));
                maxErr = juce::jmax (maxErr, std::abs ((double) o.R[(size_t) k] - gain * genR (pos)));
            }
            bool silentAfter = true;
            for (int i = len + 1; i < 6144; ++i) silentAfter &= (o.L[(size_t) i] == 0.0f);
            CHECK (maxErr < 2.0e-5, what);
            CHECK (silentAfter && eng.getNumActiveVoices() == 0, "stops at the region end");
        };

        check (1000, 3000, "mid-file region: fade in + fade out");
        check (0, 2000, "region from file start: no fade in");
        check (3000, 5000, "region to file end: no fade out");
        check (2000, 2040, "very short region");

        // the cut never produces a jump: first and last output samples are ~0 when fading
        lib.setRegion (0, 1000, 3000); eng.reset();
        auto o = run (eng, 4096, 512, [] (int b, juce::MidiBuffer& m) { if (b == 0) m.addEvent (noteOn (60, 127), 0); });
        CHECK (std::abs (o.L[0]) < 1e-6f, "fade-in starts at zero (no click)");
        CHECK (std::abs (o.L[1999]) < 0.02f, "fade-out ends near zero (no click)");
    }

    std::printf ("[engine] 44.1k file on 48k host (rate conversion, also with a region)\n");
    {
        perf::Monitor pm; SampleLibrary lib (pm); LayerParams lp (pm); SamplerEngine eng (lib, lp, pm);
        CHECK (loadAll (lib, { p1 }), "loaded");
        eng.prepare (48000.0);
        auto o = run (eng, 2048, 2048, [] (int, juce::MidiBuffer& m) { m.addEvent (noteOn (72, 127), 0); });

        const double inc = 44100.0 / 48000.0;
        const int expectedLen = (int) std::ceil (1000.0 / inc);
        double maxErr = 0.0;
        for (int k = 0; k < expectedLen; ++k)
        {
            const double pos = k * inc; const int idx = (int) pos; const float fr = (float) (pos - idx);
            const float a = genL (idx), b = idx + 1 < 1000 ? genL (idx + 1) : 0.0f;
            maxErr = juce::jmax (maxErr, (double) std::abs (o.L[(size_t) k] - (a + fr * (b - a))));
        }
        CHECK (maxErr < 1.0e-4, "linear interpolation matches reference");
        bool tailSilent = true;
        for (int k = expectedLen + 1; k < 2048; ++k) tailSilent &= (o.L[(size_t) k] == 0.0f);
        CHECK (tailSilent && eng.getNumActiveVoices() == 0, "plays 1000 * 48/44.1 frames then stops");

        lib.setRegion (0, 200, 600); eng.reset();
        auto r = run (eng, 2048, 2048, [] (int, juce::MidiBuffer& m) { m.addEvent (noteOn (72, 127), 0); });
        const int regionLen = (int) std::ceil (400.0 / inc);
        bool tail = true; for (int k = regionLen + 1; k < 2048; ++k) tail &= (r.L[(size_t) k] == 0.0f);
        CHECK (tail && std::abs (r.L[0]) < 1e-6f, "resampled region: starts silent (fade-in), stops at end");
    }

    std::printf ("[engine] mono source, polyphony, panic, note-off\n");
    {
        perf::Monitor pm; SampleLibrary lib (pm); LayerParams lp (pm); SamplerEngine eng (lib, lp, pm);
        CHECK (loadAll (lib, { p2 }), "loaded");
        eng.prepare (48000.0);
        auto o = run (eng, 256, 256, [] (int, juce::MidiBuffer& m) { m.addEvent (noteOn (10, 127), 0); });
        bool same = true; double err = 0;
        for (int i = 0; i < 256; ++i) { same &= o.L[(size_t) i] == o.R[(size_t) i]; err = juce::jmax (err, (double) std::abs (o.L[(size_t) i] - genL (i))); }
        CHECK (same && err < 1e-5, "mono: L == R == source");

        eng.reset();
        juce::AudioBuffer<float> buf (2, 128);
        juce::MidiBuffer midi;
        for (int i = 0; i < 40; ++i) midi.addEvent (noteOn (40 + i, 90), i);
        eng.process (buf, midi);
        CHECK (eng.getNumActiveVoices() == SamplerEngine::kMaxVoices, "capped at 32 voices");
        bool finite = true; for (int i = 0; i < 128; ++i) finite &= std::isfinite (buf.getSample (0, i));
        CHECK (finite, "finite output");

        juce::MidiBuffer off; off.addEvent (juce::MidiMessage::noteOff (1, 40), 0);
        eng.process (buf, off);
        CHECK (eng.getNumActiveVoices() == SamplerEngine::kMaxVoices, "note-off ignored (one-shot)");
        juce::MidiBuffer panic; panic.addEvent (juce::MidiMessage::allNotesOff (1), 5);
        eng.process (buf, panic);
        CHECK (eng.getNumActiveVoices() == 0, "all-notes-off stops voices");
        juce::MidiBuffer vel0; vel0.addEvent (noteOn (60, 0), 0);
        eng.process (buf, vel0);
        CHECK (eng.getNumActiveVoices() == 0, "note-on velocity 0 does not trigger");
    }

    std::printf ("[engine] GUI Play button trigger\n");
    {
        perf::Monitor pm; SampleLibrary lib (pm); LayerParams lp (pm); SamplerEngine eng (lib, lp, pm);
        CHECK (loadAll (lib, { p3 }), "loaded");
        pm.prepare (48000.0); eng.prepare (48000.0);

        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer none;
        eng.process (buf, none);
        CHECK (eng.getNumActiveVoices() == 0 && buf.getMagnitude (0, 512) == 0.0f, "nothing plays by itself");

        eng.requestTrigger();
        eng.process (buf, none);
        const float g = (float) SamplerEngine::kGuiVelocity / 127.0f;
        double err = 0; for (int i = 0; i < 512; ++i) err = juce::jmax (err, (double) std::abs (buf.getSample (0, i) - g * genL (i)));
        CHECK (err < 1e-5, "trigger plays the selected file from the start of the next block");
        eng.process (buf, none);
        CHECK (eng.getNumActiveVoices() == 1, "one voice per click (no repeat)");

        // clicks before prepare() are ignored; many clicks in one block are capped
        eng.reset();
        for (int i = 0; i < 10; ++i) eng.requestTrigger();
        eng.process (buf, none);
        CHECK (eng.getNumActiveVoices() == 4, "burst of clicks capped at 4 voices per block");
    }

    std::printf ("[engine] start delay\n");
    {
        perf::Monitor pm; SampleLibrary lib (pm); LayerParams lp (pm); SamplerEngine eng (lib, lp, pm);
        CHECK (loadAll (lib, { p3 }), "loaded");
        pm.prepare (48000.0); eng.prepare (48000.0);

        lib.setDelay (0, 25.0f, true);                       // 25 ms = 1200 frames at 48 kHz
        auto o = run (eng, 8192, 512, [] (int b, juce::MidiBuffer& m) { if (b == 0) m.addEvent (noteOn (60, 127), 100); });
        bool silent = true; for (int i = 0; i < 100 + 1200; ++i) silent &= (o.L[(size_t) i] == 0.0f);
        double err = 0; for (int k = 0; k < 5000; ++k) err = juce::jmax (err, (double) std::abs (o.L[(size_t) (1300 + k)] - genL (k)));
        CHECK (silent, "silent until note position + delay (spans several blocks)");
        CHECK (err < 1e-5, "sound starts exactly after the delay");
        CHECK (eng.getNumActiveVoices() == 0, "voice freed after a delayed play");

        lib.setDelay (0, 99999.0f, false);
        CHECK (lib.getSample (0)->delayMs.load() == SampleData::kMaxDelayMs, "delay clamped to the maximum");
        lib.setDelay (0, -5.0f, false);
        CHECK (lib.getSample (0)->delayMs.load() == 0.0f, "negative delay clamped to 0");
    }

    std::printf ("[timeline] outline and ruler\n");
    {
        perf::Monitor pm; SampleLibrary lib (pm);
        CHECK (loadAll (lib, { p3 }), "loaded");
        const auto s = lib.getSample (0);
        juce::Path path; std::vector<float> a, b;
        const juce::Rectangle<float> area (10.0f, 20.0f, 400.0f, 100.0f);

        waveform::buildTimelineOutline (*s, 0, 5000, 1000.0, area, path, a, b);      // 5000 frames = 104 ms of a 1000 ms window
        const auto bd = path.getBounds();
        const float expectedW = 400.0f * (5000.0f / 48.0f) / 1000.0f;                // ~41.7 px
        CHECK (! path.isEmpty() && std::abs (bd.getWidth() - expectedW) < 2.0f, "outline width = sound duration on the time axis");
        CHECK (bd.getX() >= area.getX() - 0.01f && bd.getY() >= area.getY() - 0.01f && bd.getBottom() <= area.getBottom() + 0.01f, "outline inside the area");

        waveform::buildTimelineOutline (*s, 0, 5000, 50.0, area, path, a, b);        // zoomed in: sound is longer than the window
        CHECK (std::abs (path.getBounds().getWidth() - 399.0f) < 2.0f, "long sound fills the whole window");
        waveform::buildTimelineOutline (*s, 1000, 1000, 1000.0, area, path, a, b);
        CHECK (path.isEmpty(), "empty region gives an empty outline");

        CHECK (waveform::niceTickStepMs (1000.0, 700.0, 70.0) == 100.0, "ruler step: 1000 ms over 700 px -> 100 ms");
        CHECK (waveform::niceTickStepMs (100.0, 700.0, 70.0) == 10.0, "ruler step: 100 ms over 700 px -> 10 ms");
        CHECK (waveform::niceTickStepMs (5000.0, 700.0, 70.0) == 500.0, "ruler step: 5000 ms over 700 px -> 500 ms");
        CHECK (waveform::niceTickStepMs (300.0, 700.0, 70.0) == 50.0, "ruler step rounds up to 1/2/5");
    }

    std::printf ("[history] undo / redo\n");
    {
        perf::Monitor pm; SampleLibrary lib (pm);
        CHECK (! lib.canUndo() && ! lib.canRedo(), "empty history at start");

        CHECK (loadAll (lib, { p1, p2, p3 }), "loaded 3");
        CHECK (lib.getHistoryDepth() == 1, "importing 3 files at once = ONE step");
        const auto a = lib.getSample (0), b = lib.getSample (1), c = lib.getSample (2);

        lib.undo();
        CHECK (lib.getNumLoaded() == 0 && lib.canRedo(), "undo import removes the imported files");
        lib.redo();
        CHECK (lib.getNumLoaded() == 3 && lib.getSample (0) == a && lib.getSample (2) == c, "redo brings the same audio back (no re-decoding)");

        // remove one file that has an edited region and delay, then undo
        lib.setRegion (1, 1000, 1500); lib.setDelay (1, 40.0f, true); lib.setSelected (1);
        const int depth = lib.getHistoryDepth();
        lib.removeSample (1);
        CHECK (lib.getNumLoaded() == 2 && lib.getHistoryDepth() == depth + 1, "remove is one step");
        lib.undo();
        int s, e;
        CHECK (lib.getNumLoaded() == 3 && lib.getSample (1) == b, "undo remove restores the file at its position");
        b->getRegion (s, e);
        CHECK (s == 1000 && e == 1500 && b->delayMs.load() == 40.0f, "restored file keeps its region and delay");
        CHECK (lib.getSelected() == 1, "selection restored");

        // clear all, undo
        lib.setSelected (2);
        lib.clear();
        CHECK (lib.getNumLoaded() == 0, "cleared");
        lib.undo();
        CHECK (lib.getNumLoaded() == 3 && lib.getSelected() == 2 && lib.getSample (0) == a, "undo clear restores every file");
        lib.clear(); lib.clear();                                   // second clear changes nothing
        const int d2 = lib.getHistoryDepth();
        lib.clear();
        CHECK (lib.getHistoryDepth() == d2, "a no-op is not recorded");
        lib.undo();

        // region edit and a delay drag (many steps, one undo)
        lib.setRegion (2, 2000, 3000);                              // slot 2 is the 5000-frame file
        lib.undo();
        c->getRegion (s, e);
        CHECK (s == 0 && e == 5000, "undo region edit");
        lib.redo();
        c->getRegion (s, e);
        CHECK (s == 2000 && e == 3000, "redo region edit");

        const int d3 = lib.getHistoryDepth();
        lib.setDelay (0, 10.0f, false); lib.setDelay (0, 20.0f, false); lib.setDelay (0, 30.0f, true);
        CHECK (lib.getHistoryDepth() == d3 + 1 && a->delayMs.load() == 30.0f, "a whole delay drag is one step");
        lib.undo();
        CHECK (a->delayMs.load() == 0.0f, "undo delay drag returns to the value before the drag");

        // a new edit discards the redo branch
        CHECK (lib.canRedo(), "redo available after undo");
        lib.setRegion (0, 100, 900);
        CHECK (! lib.canRedo(), "new edit clears redo");

        // history is bounded
        for (int i = 0; i < 100; ++i) lib.setRegion (0, 100 + i, 900 + i);
        CHECK (lib.getHistoryDepth() <= 40, "history limited to 40 steps");

        // loading a project is not undoable
        lib.restore ({ { p1, -1, -1, 0.0f } }, 0);
        CHECK (pumpUntil ([&] { return lib.getNumLoaded() == 1; }) && ! lib.canUndo(), "project load starts with a clean history");
    }

    std::printf ("[history] engine safety when removed audio comes back\n");
    {
        perf::Monitor pm; SampleLibrary lib (pm); LayerParams lp (pm); SamplerEngine eng (lib, lp, pm);
        CHECK (loadAll (lib, { p3 }), "loaded");
        eng.prepare (48000.0);
        juce::AudioBuffer<float> buf (2, 256); juce::MidiBuffer none, midi; midi.addEvent (noteOn (60, 127), 0);
        eng.process (buf, midi);
        lib.removeSample (0);
        eng.process (buf, none);
        CHECK (eng.getNumActiveVoices() == 0 && buf.getMagnitude (0, 256) == 0.0f, "voice of removed file dropped");
        lib.undo();
        eng.process (buf, midi);
        CHECK (eng.getNumActiveVoices() == 1 && buf.getMagnitude (0, 256) > 0.1f, "restored file plays again");
    }

    std::printf ("[waveform] zoomed outline\n");
    {
        perf::Monitor pm; SampleLibrary lib (pm);
        CHECK (loadAll (lib, { p3 }), "loaded");
        const auto smp = lib.getSample (0);
        juce::Path path; std::vector<float> a, b;
        const juce::Rectangle<float> area (0.0f, 0.0f, 400.0f, 100.0f);

        waveform::buildOutline (*smp, 1000.0, 1100, 100.0 / 400.0, area, path, a, b);        // 100 frames over 400 px: reads the audio
        CHECK (! path.isEmpty() && path.getBounds().getWidth() > 395.0f, "deep zoom fills the area");
        waveform::buildOutline (*smp, 0.0, 5000, 5000.0 / 8.0, juce::Rectangle<float> (0.0f, 0.0f, 8.0f, 100.0f), path, a, b);   // 625 frames/px: uses the peaks
        CHECK (! path.isEmpty() && path.getBounds().getWidth() > 6.0f, "zoomed-out view built from peaks");
        waveform::buildOutline (*smp, 4990.0, 5000, 0.05, area, path, a, b);                  // 10 frames only: partial window
        CHECK (! path.isEmpty() && path.getBounds().getWidth() < 250.0f, "view ending at the file end draws only what exists");
    }

    std::printf ("[engine] removing files while voices play\n");
    {
        perf::Monitor pm; SampleLibrary lib (pm); LayerParams lp (pm); SamplerEngine eng (lib, lp, pm);
        CHECK (loadAll (lib, { p1, p2, p3 }), "loaded");
        eng.prepare (48000.0);
        lib.setSelected (2);                                   // c_stereo48 (unity)
        std::weak_ptr<const SampleData> weakA = lib.getSample (0);
        std::weak_ptr<const SampleData> weakC = lib.getSample (2);

        juce::AudioBuffer<float> buf (2, 256); juce::MidiBuffer none;
        juce::MidiBuffer midi; midi.addEvent (noteOn (60, 127), 0);
        eng.process (buf, midi);                               // frames 0..255 of c
        lib.removeSample (0);                                  // remove ANOTHER file: slots shift
        eng.process (buf, none);                               // frames 256..511 must continue untouched
        double err = 0; for (int i = 0; i < 256; ++i) err = juce::jmax (err, (double) std::abs (buf.getSample (0, i) - genL (256 + i)));
        CHECK (err < 1e-5 && eng.getNumActiveVoices() == 1, "voice on a shifted file keeps playing");
        CHECK (! weakA.expired(), "removed memory retained for now");

        lib.removeSample (lib.getSelected());                  // remove the playing file itself
        CHECK (! weakC.expired(), "playing file's memory retained");
        eng.process (buf, none);
        CHECK (eng.getNumActiveVoices() == 0 && buf.getMagnitude (0, 256) == 0.0f, "voice of removed file dropped, no audio from freed data");
        eng.process (buf, none); eng.process (buf, none);
        CHECK (! weakA.expired(), "history keeps removed audio so it can be restored");
        lib.clearHistory();
        CHECK (pumpUntil ([&] { return weakA.expired() && weakC.expired(); }, 3000), "memory freed once audio moved on and history released");
    }

    std::printf ("[library] destroying while loads are in flight\n");
    {
        auto big = writeWav (dir, "big.wav", 48000.0, 2, 48000 * 20);
        perf::Monitor pm;
        auto lib = std::make_unique<SampleLibrary> (pm);
        juce::StringArray fs; for (int i = 0; i < 6; ++i) fs.add (big.getFullPathName());
        lib->addFiles (fs);
        lib.reset();
        CHECK (true, "destroyed cleanly");
    }

    // ----------------------------------------------------------------- waveform
    std::printf ("[waveform] outline\n");
    {
        perf::Monitor pm; SampleLibrary lib (pm);
        CHECK (loadAll (lib, { p3 }), "loaded");
        juce::Path path; std::vector<float> a, b;
        const juce::Rectangle<float> area (10.0f, 20.0f, 400.0f, 200.0f);
        waveform::buildOutline (*lib.getSample (0), area, path, a, b);
        const auto bounds = path.getBounds();
        CHECK (! path.isEmpty(), "outline built");
        CHECK (bounds.getX() >= area.getX() - 0.01f && bounds.getRight() <= area.getRight() + 0.01f, "stays inside the area horizontally");
        CHECK (bounds.getY() >= area.getY() - 0.01f && bounds.getBottom() <= area.getBottom() + 0.01f, "stays inside the area vertically");
        waveform::buildOutline (*lib.getSample (0), { 0.0f, 0.0f, 1.0f, 100.0f }, path, a, b);
        CHECK (path.isEmpty(), "too-narrow area gives an empty outline");
    }

    // ------------------------------------------------------------- perf monitor
    std::printf ("[perf] counters and report\n");
    {
        perf::Monitor pm; SampleLibrary lib (pm); LayerParams lp (pm); SamplerEngine eng (lib, lp, pm);
        CHECK (loadAll (lib, { p3 }), "loaded");
        pm.prepare (48000.0); eng.prepare (48000.0);
        juce::AudioBuffer<float> buf (2, 256);
        for (int b = 0; b < 20; ++b)
        {
            juce::MidiBuffer midi; if (b == 0) midi.addEvent (noteOn (60, 100), 0);
            if (b == 1) eng.requestTrigger();
            const auto t0 = perf::Monitor::now();
            eng.process (buf, midi);
            pm.recordAudioBlock (perf::Monitor::now() - t0, 256, eng.getNumActiveVoices());
        }
        auto lines = pm.buildReport();
        std::printf ("%s\n", lines.joinIntoString ("\n").toRawUTF8());
        const auto text = lines.joinIntoString ("\n");
        CHECK (lines.size() == perf::Monitor::kReportLines, "report line count");
        for (const char* name : { "processBlock", "midi", "voiceRender", "trigLatency", "fileDecode", "peakBuild",
                                  "libCommit", "libRemove", "libHistory", "editorResize", "menuBuild", "wavePath", "wavePaint", "selPaint", "tlPath", "tlPaint", "modTable", "envPaint" })
            CHECK (text.contains (name), name);
    }

    // ----------------------------------------------------------------- processor
    std::printf ("[mod] envelope model, table and lock-free hand-over\n");
    {
        const auto flat = mod::Envelope::flat (0.5f), gate = mod::Envelope::gate();
        CHECK (flat.valueAt (0.3f) == 0.5f, "flat envelope");
        CHECK (gate.valueAt (0.0f) == 1.0f && gate.valueAt (0.5f) == 1.0f && gate.valueAt (0.999f) == 1.0f && gate.valueAt (1.0f) == 0.0f, "gate: vertical edges at both ends");

        auto e = mod::Envelope::flat (0.5f);
        CHECK (e.addPoint (0.3f, 0.8f) == 2 && e.pts[2].x == 0.3f && e.pts[1].x < 0.3f && e.pts[3].x > 0.3f, "added point keeps the order");
        while (e.count < mod::kMaxPoints) e.addPoint (0.9f, 0.1f);
        CHECK (e.addPoint (0.5f, 0.5f) == -1, "at most 8 points");
        e.removePoint (2);
        CHECK (e.count == mod::kMaxPoints - 1, "remove a point");

        mod::Envelope bad; bad.count = 3; bad.pts[0] = { 0.4f, 2.0f }; bad.pts[1] = { 0.2f, -1.0f }; bad.pts[2] = { 0.5f, 0.5f };
        bad.normalize();
        CHECK (bad.pts[0].x == 0.0f && bad.pts[2].x == 1.0f && bad.pts[1].x >= bad.pts[0].x && bad.pts[0].y == 1.0f && bad.pts[1].y == 0.0f, "normalize fixes order, ends and range");

        perf::Monitor pm; LayerParams lp (pm);
        CHECK (std::abs (lp.speed.toNorm (1.0f) - 0.5) < 1e-9 && std::abs (lp.speed.fromNorm (0.75) - 2.0f) < 1e-5f, "speed: logarithmic, x1 in the middle, x2 at 75 %");
        lp.speed.setEnvelope (mod::Envelope::flat (0.75f));
        CHECK (std::abs (lp.speed.readTable().at (0.5) - 2.0f) < 1e-5f, "published table holds real speed values");
        lp.level.setKnob (7.0f); lp.speed.setKnob (0.0f);
        CHECK (lp.level.getKnob() == 0.0f && lp.speed.getKnob() == 0.25f, "knobs are clamped to their range");
        lp.level.setKnob (-99.0f);
        CHECK (lp.level.getKnob() == -60.0f && lp.level.getAudioValue() == 0.0f, "level: the bottom of the range is silence");
        lp.level.setKnob (0.0f);
        CHECK (lp.level.getAudioValue() == 1.0f, "level: 0 dB is unity gain");
        lp.level.setKnob (-6.0206f);
        CHECK (std::abs (lp.level.getAudioValue() - 0.5f) < 1e-4f, "level: -6 dB is half the amplitude");
        CHECK (std::abs (lp.level.unitsFromNorm (0.5) + 30.0f) < 1e-4f && std::abs (lp.level.toNorm (-15.0f) - 0.75) < 1e-9
               && lp.level.fromNorm (1.0) == 1.0f && lp.level.fromNorm (0.0) == 0.0f && std::abs (lp.level.fromNorm (0.5) - 0.0316228f) < 1e-5f,
               "level envelope axis: 0..1 maps to silence..-60 dB..0 dB");

        float lo = 0.0f, hi = 1.0f;
        mod::zoomRange (lo, hi, 0.5f, 0.5f);
        CHECK (std::abs (lo - 0.25f) < 1e-6f && std::abs (hi - 0.75f) < 1e-6f, "zoom in around the middle");
        mod::zoomRange (lo, hi, 0.0f, 0.5f);
        CHECK (std::abs (lo - 0.25f) < 1e-6f && std::abs (hi - 0.5f) < 1e-6f, "zoom keeps the anchor point fixed");
        for (int i = 0; i < 30; ++i) mod::zoomRange (lo, hi, 0.5f, 0.5f);
        CHECK (std::abs ((hi - lo) - 0.02f) < 1e-6f, "zoom stops at the minimum range");
        for (int i = 0; i < 30; ++i) mod::zoomRange (lo, hi, 0.5f, 2.0f);
        CHECK (lo == 0.0f && hi == 1.0f, "zooming out returns to the full range");
        lo = 0.2f; hi = 0.4f; mod::panRange (lo, hi, 5.0f);
        CHECK (std::abs (hi - 1.0f) < 1e-6f && std::abs ((hi - lo) - 0.2f) < 1e-6f, "pan stops at the edge, keeping the width");

        lp.speed.setEnvelope (mod::Envelope::flat (0.1f)); lp.level.setEnvelope (mod::Envelope::flat (0.1f));
        const auto ds = lp.speed.getDefaultEnvelope(), dl = lp.level.getDefaultEnvelope();
        CHECK (ds.count == 5 && ds.pts[2].y == 0.5f && dl.count == 5 && dl.valueAt (0.0f) == 1.0f && dl.valueAt (1.0f) == 0.0f, "default envelopes: speed flat at x1, level gate");

        // one writer, one reader, nothing ever torn
        mod::TripleBuffer<mod::EnvTable> tb; tb.fillAll ({});
        std::atomic<bool> stop { false }; int torn = 0, reads = 0; float lastSeen = 0.0f; bool backwards = false;
        std::thread writer ([&] { for (int i = 1; i <= 100000; ++i) { auto& b = tb.writeBuffer(); for (auto& v : b.v) v = (float) i; tb.publish(); } stop = true; });
        while (! stop.load()) { const auto& tbl = tb.read(); const float f = tbl.v[0]; for (const float v : tbl.v) if (v != f) ++torn; if (f < lastSeen) backwards = true; lastSeen = f; ++reads; }
        writer.join();
        CHECK (torn == 0 && ! backwards && reads > 0, "triple buffer: no torn or out-of-order reads under contention");
        CHECK (tb.read().v[0] == 100000.0f, "reader ends up with the last published value");
    }

    std::printf ("[engine] speed and level (knob and envelope)\n");
    {
        perf::Monitor pm; SampleLibrary lib (pm); LayerParams lp (pm); SamplerEngine eng (lib, lp, pm);
        CHECK (loadAll (lib, { p3 }), "loaded");
        pm.prepare (48000.0);
        auto play = [&] (int frames = 12288) { eng.prepare (48000.0); return run (eng, frames, 512, [] (int b, juce::MidiBuffer& m) { if (b == 0) m.addEvent (noteOn (60, 127), 0); }); };

        lp.speed.setKnob (2.0f);
        auto o = play();
        double err = 0; for (int i = 0; i < 2500; ++i) err = juce::jmax (err, (double) std::abs (o.L[(size_t) i] - genL (2 * i)));
        bool tail = true; for (int i = 2501; i < 4096; ++i) tail &= (o.L[(size_t) i] == 0.0f);
        CHECK (err < 1e-5 && tail && eng.getNumActiveVoices() == 0, "speed x2: plays every other frame, half as long");

        lp.speed.setKnob (0.5f);
        o = play();
        err = 0; for (int i = 0; i < 9990; ++i) { const double p = i * 0.5; const int k = (int) p; err = juce::jmax (err, std::abs ((double) o.L[(size_t) i] - (genL (k) + (p - k) * (genL (k + 1) - genL (k))))); }
        tail = true; for (int i = 10002; i < 12288; ++i) tail &= (o.L[(size_t) i] == 0.0f);
        CHECK (err < 1e-4 && tail, "speed x0.5: interpolated, twice as long");

        lp.speed.setKnob (1.0f); lp.level.setKnob (-6.0206f);
        o = play();
        err = 0; for (int i = 0; i < 5000; ++i) err = juce::jmax (err, (double) std::abs (o.L[(size_t) i] - 0.5f * genL (i)));
        CHECK (err < 1e-5, "level 0.5 halves the output");

        // moving the knob is smoothed over the block (no zipper noise)
        lp.level.setKnob (0.0f);
        eng.prepare (48000.0);
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer none, on; on.addEvent (noteOn (60, 127), 0);
        eng.process (buf, on);
        lp.level.setKnob (-6.0206f);
        eng.process (buf, none);
        err = 0; for (int k = 0; k < 512; ++k) err = juce::jmax (err, (double) std::abs (buf.getSample (0, k) - (1.0f - 0.5f * (float) k / 512.0f) * genL (512 + k)));
        CHECK (err < 1e-5, "level change ramps linearly across the block");
        eng.process (buf, none);
        CHECK (std::abs (buf.getSample (0, 5) - 0.5f * genL (1024 + 5)) < 1e-5f, "then holds the new level");

        // level envelope: a straight line in dB from 0 dB to silence over the sound
        lp.level.setKnob (0.0f);
        mod::Envelope down; down.count = 2; down.pts[0] = { 0.0f, 1.0f }; down.pts[1] = { 1.0f, 0.0f };
        lp.level.setEnvelope (down); lp.level.setEnvelopeMode (true);
        o = play();
        err = 0; for (int i = 0; i < 4990; ++i) err = juce::jmax (err, (double) std::abs (o.L[(size_t) i] - std::pow (10.0, -3.0 * (double) i / 5000.0) * genL (i)));
        CHECK (err < 5e-4, "level envelope follows the sound position, linear in dB");
        lp.level.setEnvelopeMode (false);

        // speed envelope: flat at x1 changes nothing, flat at x2 equals the x2 knob
        lp.speed.setEnvelope (mod::Envelope::flat (0.5f)); lp.speed.setEnvelopeMode (true);
        o = play();
        err = 0; for (int i = 0; i < 5000; ++i) err = juce::jmax (err, (double) std::abs (o.L[(size_t) i] - genL (i)));
        CHECK (err < 1e-5, "speed envelope flat at x1 = unmodulated playback");
        lp.speed.setEnvelope (mod::Envelope::flat (0.75f));
        o = play();
        err = 0; for (int i = 0; i < 2480; ++i) err = juce::jmax (err, (double) std::abs (o.L[(size_t) i] - genL (2 * i)));
        CHECK (err < 1e-4 && eng.getNumActiveVoices() == 0, "speed envelope flat at x2 = the x2 knob");

        // a wobbling speed envelope always terminates and never produces garbage
        mod::Envelope wob; wob.count = 5; const float ys[] = { 0.1f, 0.9f, 0.3f, 1.0f, 0.0f };
        for (int i = 0; i < 5; ++i) wob.pts[(size_t) i] = { 0.25f * (float) i, ys[i] };
        lp.speed.setEnvelope (wob);
        o = play (65536);
        bool finite = true; for (const float v : o.L) finite &= std::isfinite (v) && std::abs (v) <= 1.0f;
        CHECK (finite && eng.getNumActiveVoices() == 0, "wobbling speed envelope: finite output, voice ends");
    }

    std::printf ("[processor] state round trip (files, region, selection, window size)\n");
    {
        juce::MemoryBlock state;
        {
            SamplerProcessor p;
            p.getLibrary().addFiles ({ p1, p3 });
            pumpUntil ([&] { return p.getLibrary().getNumLoaded() == 2; });
            p.getLibrary().setSelected (1);
            p.getLibrary().setRegion (1, 1000, 3000);
            p.getLibrary().setDelay (1, 250.0f, true);
            p.setScaleMs (2500);
            p.getLayer().speed.setKnob (1.5f);
            p.getLayer().level.setKnob (-3.5f);
            { mod::Envelope e = mod::Envelope::flat (0.5f); e.addPoint (0.4f, 0.9f); p.getLayer().speed.setEnvelope (e); p.getLayer().speed.setEnvelopeMode (true); }
            p.setEditorSize (900, 500);
            p.getStateInformation (state);
        }
        SamplerProcessor p2;
        p2.setStateInformation (state.getData(), (int) state.getSize());
        CHECK (pumpUntil ([&] { return p2.getLibrary().getNumLoaded() == 2; }), "files restored");
        CHECK (p2.getLibrary().getSelected() == 1, "selection restored");
        int s, e; p2.getLibrary().getSample (1)->getRegion (s, e);
        CHECK (s == 1000 && e == 3000, "region restored");
        p2.getLibrary().getSample (0)->getRegion (s, e);
        CHECK (s == 0 && e == 1000, "untouched file still plays whole");
        CHECK (p2.getEditorWidth() == 900 && p2.getEditorHeight() == 500, "window size restored");
        CHECK (p2.getLibrary().getSample (1)->delayMs.load() == 250.0f && p2.getLibrary().getSample (0)->delayMs.load() == 0.0f, "per-file delay restored");
        CHECK (p2.getScaleMs() == 2500, "timeline scale restored");
        const auto e2 = p2.getLayer().speed.getEnvelope();
        CHECK (std::abs (p2.getLayer().speed.getKnob() - 1.5f) < 1e-6f && std::abs (p2.getLayer().level.getKnob() + 3.5f) < 1e-6f, "speed and level knobs restored");
        CHECK (p2.getLayer().speed.isEnvelopeMode() && ! p2.getLayer().level.isEnvelopeMode() && e2.count == 6 && std::abs (e2.pts[2].y - 0.9f) < 1e-6f, "envelope mode and points restored");

        p2.setPlayConfigDetails (0, 2, 48000.0, 256);
        p2.prepareToPlay (48000.0, 256);
        juce::AudioBuffer<float> buf (2, 256);
        p2.getLibrary().setDelay (1, 0.0f, false);          // the restored 250 ms delay would keep the first block silent
        p2.triggerFromGui();
        juce::MidiBuffer none;
        p2.processBlock (buf, none);
        CHECK (buf.getMagnitude (0, 256) > 0.0f, "processor renders audio for the Play button");
        CHECK (p2.isBusesLayoutSupported (p2.getBusesLayout()), "default layout supported");
    }

    std::printf ("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
