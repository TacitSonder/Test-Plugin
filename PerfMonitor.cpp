#include "PerfMonitor.h"

#if SAMPLER_PERF_ENABLED

namespace perf
{
    namespace
    {
        juce::String fmtTime (double microseconds)
        {
            const juce::String s = microseconds >= 1000.0 ? juce::String (microseconds / 1000.0, 2) + " ms"
                                                          : juce::String (microseconds, 1) + " us";
            return s.paddedLeft (' ', 10);
        }

        const char* groupTitle (Thread t)
        {
            switch (t)
            {
                case Thread::Audio:   return "-- audio thread --";
                case Thread::Loader:  return "-- loader threads --";
                case Thread::Message: return "-- UI (message thread) --";
            }
            return "";
        }
    }

    juce::StringArray Monitor::buildReport()
    {
        const double toUs = 1.0e6 / ticksPerSecond;

        std::array<Counter::Snapshot, (size_t) Id::Count> snaps;
        for (size_t i = 0; i < snaps.size(); ++i)
            snaps[i] = counters[i].takeWindow();

        const auto budget = budgetTicks.exchange (0);
        const auto& proc = snaps[(size_t) Id::ProcessBlock];
        const double avgLoad = budget > 0 ? 100.0 * (double) proc.total / (double) budget : 0.0;
        const double maxLoad = windowPeakLoad.exchange (0) / 100.0;
        const double peakLoad = sessionPeakLoad.load() / 100.0;

        juce::StringArray lines;
        lines.add ("load avg " + juce::String (avgLoad, 2) + "% max " + juce::String (maxLoad, 1)
                   + "% peak " + juce::String (peakLoad, 1) + "% | xruns " + juce::String ((int) overruns.load())
                   + " | voices " + juce::String (windowMaxVoices.exchange (0))
                   + " (peak " + juce::String (sessionMaxVoices.load()) + ")");
        lines.add (juce::String().paddedRight (' ', 13) + "       avg       max      peak  calls");

        for (auto group : { Thread::Audio, Thread::Loader, Thread::Message })
        {
            lines.add (groupTitle (group));

            for (size_t i = 0; i < snaps.size(); ++i)
            {
                if (kInfo[i].thread != group)
                    continue;

                const auto& s = snaps[i];
                const double avg = s.count > 0 ? (double) s.total / (double) s.count * toUs : 0.0;
                const double mx  = (double) s.windowMax * toUs;
                const double pk  = (double) s.sessionMax * toUs;

                lines.add (juce::String (kInfo[i].name).paddedRight (' ', 13)
                           + fmtTime (avg) + fmtTime (mx) + fmtTime (pk)
                           + juce::String ((juce::int64) s.count).paddedLeft (' ', 6));
            }
        }

        jassert (lines.size() == kReportLines);
        return lines;
    }
}

#else
// Nothing to compile: the disabled Monitor is header-only.
namespace perf { [[maybe_unused]] static const int perfMonitorDisabled = 0; }
#endif
