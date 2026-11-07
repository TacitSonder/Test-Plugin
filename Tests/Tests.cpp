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
        CHECK (lib.getSample (0)->numPeakBuckets == 1000, "peak buckets = frames when short");
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
        perf::Monitor pm; SampleLibrary lib (pm); SamplerEngine eng (lib, pm);
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
        perf::Monitor pm; SampleLibrary lib (pm); SamplerEngine eng (lib, pm);
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
        perf::Monitor pm; SampleLibrary lib (pm); SamplerEngine eng (lib, pm);
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
        perf::Monitor pm; SampleLibrary lib (pm); SamplerEngine eng (lib, pm);
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
        perf::Monitor pm; SampleLibrary lib (pm); SamplerEngine eng (lib, pm);
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

    std::printf ("[engine] removing files while voices play\n");
    {
        perf::Monitor pm; SampleLibrary lib (pm); SamplerEngine eng (lib, pm);
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
        CHECK (pumpUntil ([&] { return weakA.expired() && weakC.expired(); }, 3000), "memory freed once audio moved on");
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
        perf::Monitor pm; SampleLibrary lib (pm); SamplerEngine eng (lib, pm);
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
                                  "libCommit", "libRemove", "editorResize", "menuBuild", "wavePath", "wavePaint", "selPaint" })
            CHECK (text.contains (name), name);
    }

    // ----------------------------------------------------------------- processor
    std::printf ("[processor] state round trip (files, region, selection, window size)\n");
    {
        juce::MemoryBlock state;
        {
            SamplerProcessor p;
            p.getLibrary().addFiles ({ p1, p3 });
            pumpUntil ([&] { return p.getLibrary().getNumLoaded() == 2; });
            p.getLibrary().setSelected (1);
            p.getLibrary().setRegion (1, 1000, 3000);
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

        p2.setPlayConfigDetails (0, 2, 48000.0, 256);
        p2.prepareToPlay (48000.0, 256);
        juce::AudioBuffer<float> buf (2, 256);
        p2.triggerFromGui();
        juce::MidiBuffer none;
        p2.processBlock (buf, none);
        CHECK (buf.getMagnitude (0, 256) > 0.0f, "processor renders audio for the Play button");
        CHECK (p2.isBusesLayoutSupported (p2.getBusesLayout()), "default layout supported");
    }

    std::printf ("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
