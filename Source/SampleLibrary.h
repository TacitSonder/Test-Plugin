#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include "PerfMonitor.h"
#include "SampleData.h"

#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <vector>

// Owns up to kMaxSamples decoded files.
//
//  Threading model
//  ---------------
//  * Decoding runs on a small background pool, never on the UI or audio thread.
//  * Results are committed in *drop order* on the message thread.
//  * The audio thread only ever reads raw `const SampleData*` from audioSlots
//    (lock-free, no reference counting, no allocation).
//  * When a sample is removed, its memory is kept alive ("retired") until the
//    audio thread has started two more blocks, so a block that was in flight at
//    the moment of removal can never touch freed memory. The audio engine drops
//    any voice whose sample was removed at the start of each block.
class SampleLibrary : private juce::AsyncUpdater,
                      private juce::Timer
{
public:
    static constexpr int kMaxSamples = 8;

    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void sampleLibraryChanged() = 0;   // message thread
    };

    // What is saved in the project for each file.
    struct FileState
    {
        juce::String path;
        int start = -1;     // -1 = whole file
        int end = -1;
    };

    explicit SampleLibrary (perf::Monitor&);
    ~SampleLibrary() override;

    // ---- message thread only ------------------------------------------------
    bool isSupportedFile (const juce::File&) const;               // cheap extension test
    int  addFiles (const juce::StringArray& paths);                // returns number queued
    void restore (const std::vector<FileState>& files, int selectedIndex);
    void clear();
    void removeSample (int slot);                                   // remove one file, later files move up
    void setRegion (int slot, int start, int end);                  // which part of the file plays
    void setSelected (int slot);
    int  getSelected() const noexcept;                              // clamped for display
    int  getNumLoaded() const noexcept   { return numLoaded; }
    int  getNumPending() const noexcept  { return (int) pending.size(); }
    std::shared_ptr<const SampleData> getSample (int slot) const;
    size_t getTotalBytes() const;
    std::vector<FileState> getState() const;                        // loaded + still loading
    const juce::String& getStatusMessage() const noexcept { return statusMessage; }
    void addListener (Listener* l)     { listeners.add (l); }
    void removeListener (Listener* l)  { listeners.remove (l); }

    // ---- audio thread (lock-free, allocation-free) --------------------------
    void beginAudioBlock() noexcept    { audioBlockCounter.fetch_add (1, std::memory_order_acq_rel); }
    const SampleData* getForAudio (int slot) const noexcept
    {
        return audioSlots[(size_t) juce::jlimit (0, kMaxSamples - 1, slot)].load (std::memory_order_acquire);
    }
    int getSelectedForAudio() const noexcept { return selected.load (std::memory_order_relaxed); }

private:
    struct LoadTask
    {
        juce::File file;
        int regionStart = -1, regionEnd = -1;     // restored selection (-1 = whole file)
        std::shared_ptr<SampleData> result;
        juce::String error;
        std::atomic<bool> cancelled { false };
        std::atomic<bool> done { false };
    };

    struct Retired
    {
        std::shared_ptr<const SampleData> data;
        juce::uint64 epoch;
    };

    struct QueueCounts { int queued = 0, unsupported = 0, ignored = 0; };

    void queueFile (const FileState&, QueueCounts&);
    void finishQueueing (const QueueCounts&);
    void handleAsyncUpdate() override;   // message thread: commit finished loads
    void timerCallback() override;       // message thread: free retired samples
    void runLoad (LoadTask&);            // loader thread
    std::shared_ptr<SampleData> decode (LoadTask&);
    juce::ThreadPool& getPool();
    void retire (std::shared_ptr<const SampleData>);
    void notify();

    perf::Monitor& perfMon;
    juce::AudioFormatManager formats;     // fully registered in the constructor, read-only afterwards
    juce::StringArray extensions;         // lower-case, e.g. ".wav"
    std::unique_ptr<juce::ThreadPool> pool;   // created on first use

    // message-thread state
    std::array<std::shared_ptr<const SampleData>, kMaxSamples> slots;
    int numLoaded = 0;
    std::deque<std::shared_ptr<LoadTask>> pending;
    std::vector<Retired> retired;
    juce::String statusMessage;
    juce::ListenerList<Listener> listeners;

    // shared with the audio thread
    std::array<std::atomic<const SampleData*>, kMaxSamples> audioSlots;
    std::atomic<int> selected { 0 };
    std::atomic<juce::uint64> audioBlockCounter { 0 };

    JUCE_DECLARE_WEAK_REFERENCEABLE (SampleLibrary)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampleLibrary)
};
