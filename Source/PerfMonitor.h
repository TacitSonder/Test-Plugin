#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <atomic>
#include <iterator>

// -----------------------------------------------------------------------------
//  Performance monitor
//
//  * Every measured section has a fixed slot in perf::Id, so recording a timing
//    is an array index plus a few relaxed atomic operations: no locks, no
//    allocation, safe to call from the audio thread.
//  * Each counter keeps a "window" (reset every time the report is built) and a
//    "session" peak (reset with resetSession()).
//  * Compile with SAMPLER_PERF_ENABLED=0 (CMake option SAMPLER_PERF_MONITOR=OFF)
//    and every call below compiles to nothing.
//
//  To measure something new: add an entry to perf::Id and perf::kInfo, then put
//  `perf::ScopedTimer t (monitor, perf::Id::YourThing);` at the top of the code.
// -----------------------------------------------------------------------------

#ifndef SAMPLER_PERF_ENABLED
 #define SAMPLER_PERF_ENABLED 1
#endif

namespace perf
{
    enum class Id : int
    {
        // audio thread
        ProcessBlock = 0,   // whole processBlock()
        Midi,               // MIDI parsing + voice start (only blocks that contained MIDI)
        VoiceRender,        // sample playback mixing (only blocks with active voices)
        TriggerLatency,     // Play-button click -> start of the audio block that handles it
        // loader threads
        FileDecode,         // decode one file from disk
        PeakBuild,          // build the waveform peaks of one file
        // message (UI) thread
        LibraryCommit,      // publish finished loads to the audio thread
        LibraryRemove,      // remove one file
        HistoryOp,          // record / undo / redo one history step
        ModTable,           // rebuild + publish a modulation envelope (every edit step)
        EditorResize,       // editor resized(), including child layout
        MenuBuild,          // rebuild the file dropdown
        WaveformPath,       // rebuild the waveform outline
        WaveformPaint,      // waveform paint()
        SelectionPaint,     // selection overlay paint() (runs on every drag step)
        EnvPaint,           // envelope editor paint()
        TimelinePath,       // rebuild the timeline waveform (scale / region / size change)
        TimelinePaint,      // timeline paint() (runs on every delay-drag step)
        Count
    };

    enum class Thread { Audio, Loader, Message };

    struct Info { const char* name; Thread thread; };

    inline constexpr Info kInfo[] =
    {
        { "processBlock", Thread::Audio   },
        { "midi",         Thread::Audio   },
        { "voiceRender",  Thread::Audio   },
        { "trigLatency",  Thread::Audio   },
        { "fileDecode",   Thread::Loader  },
        { "peakBuild",    Thread::Loader  },
        { "libCommit",    Thread::Message },
        { "libRemove",    Thread::Message },
        { "libHistory",   Thread::Message },
        { "modTable",     Thread::Message },
        { "editorResize", Thread::Message },
        { "menuBuild",    Thread::Message },
        { "wavePath",     Thread::Message },
        { "wavePaint",    Thread::Message },
        { "selPaint",     Thread::Message },
        { "envPaint",     Thread::Message },
        { "tlPath",       Thread::Message },
        { "tlPaint",      Thread::Message },
    };
    static_assert (std::size (kInfo) == (size_t) Id::Count, "perf::kInfo must match perf::Id");

#if SAMPLER_PERF_ENABLED

    template <typename T>
    inline void raiseMax (std::atomic<T>& target, T value) noexcept
    {
        auto current = target.load (std::memory_order_relaxed);
        while (value > current && ! target.compare_exchange_weak (current, value, std::memory_order_relaxed)) {}
    }

    // One measured section. alignas(64) keeps counters written by different
    // threads on different cache lines (no false sharing).
    class alignas (64) Counter
    {
    public:
        struct Snapshot { juce::int64 count, total, windowMax, last, sessionMax; };

        void record (juce::int64 ticks) noexcept
        {
            count.fetch_add (1, std::memory_order_relaxed);
            total.fetch_add (ticks, std::memory_order_relaxed);
            last.store (ticks, std::memory_order_relaxed);
            raiseMax (windowMax, ticks);
            raiseMax (sessionMax, ticks);
        }

        // Reader side: returns the window and starts a new one.
        Snapshot takeWindow() noexcept
        {
            return { count.exchange (0), total.exchange (0), windowMax.exchange (0),
                     last.load(), sessionMax.load() };
        }

        void resetSession() noexcept { sessionMax.store (0); }

    private:
        std::atomic<juce::int64> count { 0 }, total { 0 }, last { 0 }, windowMax { 0 }, sessionMax { 0 };
    };

    class Monitor
    {
    public:
        // Number of lines buildReport() returns (the overlay sizes itself from this).
        static constexpr int kReportLines = 2 + 3 + (int) Id::Count;

        Monitor() noexcept { prepare (44100.0); }

        static juce::int64 now() noexcept { return juce::Time::getHighResolutionTicks(); }

        void prepare (double sampleRate) noexcept
        {
            ticksPerSecond = (double) juce::Time::getHighResolutionTicksPerSecond();
            ticksPerSample = ticksPerSecond / juce::jmax (1.0, sampleRate);
        }

        void record (Id id, juce::int64 ticks) noexcept { counters[(size_t) id].record (ticks); }

        // Audio thread, once per block. Also tracks DSP load = time used / time available.
        void recordAudioBlock (juce::int64 ticks, int numSamples, int activeVoices) noexcept
        {
            counters[(size_t) Id::ProcessBlock].record (ticks);

            const auto budget = (juce::int64) ((double) numSamples * ticksPerSample);
            budgetTicks.fetch_add (budget, std::memory_order_relaxed);

            if (budget > 0)
            {
                if (ticks > budget)
                    overruns.fetch_add (1, std::memory_order_relaxed);

                const int load = (int) juce::jmin ((juce::int64) 1000000, ticks * 10000 / budget); // 0.01 % units
                raiseMax (windowPeakLoad, load);
                raiseMax (sessionPeakLoad, load);
            }

            raiseMax (windowMaxVoices, activeVoices);
            raiseMax (sessionMaxVoices, activeVoices);
        }

        juce::uint32 getOverruns() const noexcept { return overruns.load(); }

        void resetSession() noexcept
        {
            for (auto& c : counters) c.resetSession();
            overruns.store (0);
            sessionPeakLoad.store (0);
            sessionMaxVoices.store (0);
        }

        // Message thread. Builds kReportLines lines and starts a new window.
        juce::StringArray buildReport();

    private:
        std::array<Counter, (size_t) Id::Count> counters;
        std::atomic<juce::int64> budgetTicks { 0 };
        std::atomic<int> windowPeakLoad { 0 }, sessionPeakLoad { 0 };
        std::atomic<int> windowMaxVoices { 0 }, sessionMaxVoices { 0 };
        std::atomic<juce::uint32> overruns { 0 };
        double ticksPerSecond = 1.0, ticksPerSample = 1.0;
    };

    class ScopedTimer
    {
    public:
        ScopedTimer (Monitor& m, Id i) noexcept : monitor (m), id (i), start (Monitor::now()) {}
        ~ScopedTimer() { monitor.record (id, Monitor::now() - start); }

    private:
        Monitor& monitor;
        Id id;
        juce::int64 start;

        ScopedTimer (const ScopedTimer&) = delete;
        ScopedTimer& operator= (const ScopedTimer&) = delete;
    };

#else // ---- disabled: same interface, zero cost ----

    class Monitor
    {
    public:
        static constexpr int kReportLines = 1;
        static juce::int64 now() noexcept { return 0; }
        void prepare (double) noexcept {}
        void record (Id, juce::int64) noexcept {}
        void recordAudioBlock (juce::int64, int, int) noexcept {}
        juce::uint32 getOverruns() const noexcept { return 0; }
        void resetSession() noexcept {}
        juce::StringArray buildReport() { return { "perf monitor disabled at build time" }; }
    };

    class ScopedTimer
    {
    public:
        ScopedTimer (Monitor&, Id) noexcept {}
    };

#endif
}
