#include "SampleLibrary.h"

namespace
{
    constexpr int    kReadChunkFrames = 1 << 16;   // decode in chunks so a cancel is noticed quickly
    constexpr double kMaxFileSeconds  = 600.0;     // guards against accidentally dropping huge files

    // One (min, max) pair per kPeakFrames frames: lets the waveform view draw any zoom level
    // without scanning the audio, and costs only ~0.4 % extra memory.
    void buildPeaks (SampleData& d)
    {
        const int n  = d.numFrames;
        const int nb = (n + SampleData::kPeakFrames - 1) / SampleData::kPeakFrames;
        d.numPeakBuckets = nb;
        d.peaks.assign ((size_t) nb * 2, 0.0f);

        const float* l = d.audio.getReadPointer (0);
        const float* r = d.numChannels > 1 ? d.audio.getReadPointer (1) : nullptr;

        for (int i = 0; i < nb; ++i)
        {
            const int s = i * SampleData::kPeakFrames;
            const int count = juce::jmin (SampleData::kPeakFrames, n - s);

            auto range = juce::FloatVectorOperations::findMinAndMax (l + s, count);
            float mn = range.getStart(), mx = range.getEnd();

            if (r != nullptr)
            {
                auto rr = juce::FloatVectorOperations::findMinAndMax (r + s, count);
                mn = juce::jmin (mn, rr.getStart());
                mx = juce::jmax (mx, rr.getEnd());
            }

            d.peaks[(size_t) i * 2]     = mn;
            d.peaks[(size_t) i * 2 + 1] = mx;
        }
    }

    juce::String plural (int n, const char* word)
    {
        return juce::String (n) + " " + word + (n == 1 ? "" : "s");
    }
}

// -----------------------------------------------------------------------------
SampleLibrary::SampleLibrary (perf::Monitor& pm) : perfMon (pm)
{
    formats.registerBasicFormats();

    for (auto& a : audioSlots)
        a.store (nullptr);

    for (auto token : juce::StringArray::fromTokens (formats.getWildcardForAllFormats(), ";", ""))
    {
        token = token.trim().replace ("*", "").toLowerCase();
        if (token.isNotEmpty())
            extensions.addIfNotAlreadyThere (token);
    }
}

SampleLibrary::~SampleLibrary()
{
    stopTimer();

    for (auto& t : pending)
        t->cancelled.store (true);

    pool.reset();            // joins the loader threads (jobs may still call triggerAsyncUpdate)
    cancelPendingUpdate();
}

juce::ThreadPool& SampleLibrary::getPool()
{
    if (pool == nullptr)
        pool = std::make_unique<juce::ThreadPool> (juce::ThreadPoolOptions{}
                    .withThreadName ("SampleDrop loader")
                    .withNumberOfThreads (juce::jlimit (1, 4, juce::SystemStats::getNumCpus() / 2))
                    .withDesiredThreadPriority (juce::Thread::Priority::low));
    return *pool;
}

// ---- message thread API -----------------------------------------------------
bool SampleLibrary::isSupportedFile (const juce::File& f) const
{
    return extensions.contains (f.getFileExtension().toLowerCase()) && f.existsAsFile();
}

int SampleLibrary::addFiles (const juce::StringArray& paths)
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());

    statusMessage = {};
    QueueCounts counts;

    for (const auto& p : paths)
        queueFile ({ p, -1, -1 }, counts);

    finishQueueing (counts);
    return counts.queued;
}

void SampleLibrary::queueFile (const FileState& fs, QueueCounts& counts)
{
    const juce::File f (fs.path);

    if (! isSupportedFile (f))
    {
        ++counts.unsupported;
        return;
    }

    if (numLoaded + (int) pending.size() >= kMaxSamples)
    {
        ++counts.ignored;
        return;
    }

    if (pending.empty() && ! importBefore)
        importBefore = makeSnapshot();

    auto task = std::make_shared<LoadTask>();
    task->file = f;
    task->regionStart = fs.start;
    task->regionEnd = fs.end;
    task->delayMs = fs.delayMs;
    pending.push_back (task);
    getPool().addJob ([this, task] { runLoad (*task); });
    ++counts.queued;
}

void SampleLibrary::finishQueueing (const QueueCounts& counts)
{
    if (counts.ignored > 0)
        statusMessage = plural (counts.ignored, "file") + " ignored: the limit is " + juce::String (kMaxSamples)
                        + " files (remove one or use Clear)";

    if (counts.unsupported > 0)
    {
        if (statusMessage.isNotEmpty())
            statusMessage << "; ";
        statusMessage << plural (counts.unsupported, "file") << " skipped (not a supported audio file)";
    }

    notify();
}

void SampleLibrary::restore (const std::vector<FileState>& files, int selectedIndex)
{
    clearInternal (false);

    QueueCounts counts;
    for (const auto& f : files)
        queueFile (f, counts);

    importBefore.reset();           // loading a project is not an undoable import
    clearHistory();
    finishQueueing (counts);
    selected.store (juce::jlimit (0, kMaxSamples - 1, selectedIndex));
}

void SampleLibrary::clear()
{
    clearInternal (true);
}

void SampleLibrary::clearInternal (bool recordHistory)
{
    auto before = makeSnapshot();
    importBefore.reset();

    for (auto& t : pending)
        t->cancelled.store (true);
    pending.clear();

    // 1) unpublish from the audio thread, 2) retire the memory
    for (auto& a : audioSlots)
        a.store (nullptr);

    for (int i = 0; i < numLoaded; ++i)
    {
        slots[(size_t) i]->removed.store (true, std::memory_order_release);   // audio thread drops its voices
        retire (std::move (slots[(size_t) i]));
    }

    numLoaded = 0;
    selected.store (0);
    statusMessage = {};

    if (recordHistory)
        pushHistory (std::move (before));

    notify();
}

void SampleLibrary::removeSample (int slot)
{
    if (slot < 0 || slot >= numLoaded)
        return;

    perf::ScopedTimer timer (perfMon, perf::Id::LibraryRemove);

    auto before = makeSnapshot();
    auto gone = std::move (slots[(size_t) slot]);

    // 1) tell the audio thread to drop voices that play it, 2) close the gap,
    // 3) keep the memory alive until the audio thread can no longer touch it.
    gone->removed.store (true, std::memory_order_release);

    for (int i = slot; i < numLoaded - 1; ++i)
    {
        slots[(size_t) i] = std::move (slots[(size_t) i + 1]);
        audioSlots[(size_t) i].store (slots[(size_t) i].get(), std::memory_order_release);
    }

    --numLoaded;
    slots[(size_t) numLoaded] = nullptr;
    audioSlots[(size_t) numLoaded].store (nullptr, std::memory_order_release);

    // Keep the same *file* selected where possible; if the selected one was removed,
    // the file that moved into its place becomes selected.
    int sel = selected.load();
    if (slot < sel)
        --sel;
    selected.store (juce::jlimit (0, juce::jmax (0, numLoaded - 1), sel));

    statusMessage = {};
    retire (std::move (gone));
    pushHistory (std::move (before));
    notify();
}

void SampleLibrary::setRegion (int slot, int start, int end)
{
    if (slot < 0 || slot >= numLoaded)
        return;

    const auto& sample = slots[(size_t) slot];
    region::sanitize (start, end, sample->numFrames);

    auto before = makeSnapshot();
    sample->setRegion (start, end);
    pushHistory (std::move (before));    // skipped automatically if nothing changed
    notify();
}

void SampleLibrary::setDelay (int slot, float ms, bool commit)
{
    if (slot < 0 || slot >= numLoaded)
        return;

    if (! dragBefore)                    // first step of a drag: remember the state to undo to
        dragBefore = makeSnapshot();

    slots[(size_t) slot]->delayMs.store (juce::jlimit (0.0f, SampleData::kMaxDelayMs, ms), std::memory_order_relaxed);

    if (commit)
    {
        auto before = std::move (*dragBefore);
        pushHistory (std::move (before));
        notify();
    }
}

void SampleLibrary::setSelected (int slot)
{
    if (numLoaded <= 0)
        return;

    slot = juce::jlimit (0, numLoaded - 1, slot);

    if (selected.exchange (slot) != slot)
        notify();
}

int SampleLibrary::getSelected() const noexcept
{
    return juce::jlimit (0, juce::jmax (0, numLoaded - 1), selected.load());
}

std::shared_ptr<const SampleData> SampleLibrary::getSample (int slot) const
{
    return (slot >= 0 && slot < numLoaded) ? slots[(size_t) slot] : nullptr;
}

size_t SampleLibrary::getTotalBytes() const
{
    size_t total = 0;
    for (int i = 0; i < numLoaded; ++i)
        total += slots[(size_t) i]->getMemoryBytes();
    return total;
}

std::vector<SampleLibrary::FileState> SampleLibrary::getState() const
{
    std::vector<FileState> state;
    state.reserve ((size_t) numLoaded + pending.size());

    for (int i = 0; i < numLoaded; ++i)
    {
        const auto& sample = slots[(size_t) i];
        FileState fs;
        fs.path = sample->path;
        sample->getRegion (fs.start, fs.end);
        fs.delayMs = sample->delayMs.load();
        state.push_back (std::move (fs));
    }

    for (auto& t : pending)
        state.push_back ({ t->file.getFullPathName(), t->regionStart, t->regionEnd, t->delayMs });

    return state;
}

void SampleLibrary::notify()
{
    listeners.call ([] (Listener& l) { l.sampleLibraryChanged(); });
}

// ---- loading ----------------------------------------------------------------
void SampleLibrary::runLoad (LoadTask& task)
{
    if (! task.cancelled.load())
        task.result = decode (task);

    task.done.store (true, std::memory_order_release);
    triggerAsyncUpdate();   // thread-safe; coalesces multiple completions
}

std::shared_ptr<SampleData> SampleLibrary::decode (LoadTask& task)
{
    perf::ScopedTimer timer (perfMon, perf::Id::FileDecode);

    try
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (task.file));

        if (reader == nullptr)                  { task.error = "unsupported or unreadable file"; return nullptr; }
        if (reader->lengthInSamples <= 0)       { task.error = "file is empty"; return nullptr; }
        if (reader->sampleRate <= 0.0
            || (double) reader->lengthInSamples / reader->sampleRate > kMaxFileSeconds)
                                                { task.error = "longer than 10 minutes"; return nullptr; }

        const int numFrames = (int) reader->lengthInSamples;
        const int numCh     = juce::jmin ((int) reader->numChannels, 2);

        auto data = std::make_shared<SampleData>();
        data->name          = task.file.getFileName();
        data->path          = task.file.getFullPathName();
        data->sampleRate    = reader->sampleRate;
        data->numFrames     = numFrames;
        data->numChannels   = numCh;
        data->audio.setSize (numCh, numFrames + SampleData::kPadFrames);
        data->setRegion (0, numFrames);

        for (int pos = 0; pos < numFrames; pos += kReadChunkFrames)
        {
            if (task.cancelled.load (std::memory_order_relaxed))
                return nullptr;

            const int num = juce::jmin (kReadChunkFrames, numFrames - pos);

            if (! reader->read (&data->audio, pos, num, pos, true, true))
            {
                task.error = "read error";
                return nullptr;
            }
        }

        data->audio.clear (numFrames, SampleData::kPadFrames);   // zero the padding

        {
            perf::ScopedTimer peakTimer (perfMon, perf::Id::PeakBuild);
            buildPeaks (*data);
        }

        return data;
    }
    catch (const std::bad_alloc&)
    {
        task.error = "not enough memory";
        return nullptr;
    }
}

void SampleLibrary::handleAsyncUpdate()
{
    perf::ScopedTimer timer (perfMon, perf::Id::LibraryCommit);

    bool changed = false;

    // Commit strictly in drop order: stop at the first task that is not finished yet.
    while (! pending.empty() && pending.front()->done.load (std::memory_order_acquire))
    {
        auto task = std::move (pending.front());
        pending.pop_front();
        changed = true;

        if (task->result != nullptr)
        {
            const auto slot = (size_t) numLoaded++;
            slots[slot] = std::move (task->result);

            if (task->regionStart >= 0 && task->regionEnd > task->regionStart)
            {
                int rs = task->regionStart, re = task->regionEnd;
                region::sanitize (rs, re, slots[slot]->numFrames);
                slots[slot]->setRegion (rs, re);
            }

            slots[slot]->delayMs.store (task->delayMs);
            audioSlots[slot].store (slots[slot].get(), std::memory_order_release);
        }
        else if (! task->cancelled.load())
        {
            if (statusMessage.isNotEmpty())
                statusMessage << "; ";
            statusMessage << "could not load " << task->file.getFileName() << " (" << task->error << ")";
        }
    }

    if (pending.empty() && importBefore)
    {
        pushHistory (std::move (*importBefore));
        importBefore.reset();
    }

    if (pending.empty() && selected.load() >= numLoaded)
    {
        selected.store (juce::jmax (0, numLoaded - 1));
        changed = true;
    }

    if (changed)
        notify();
}

// ---- deferred freeing of samples the audio thread might still be reading -----
void SampleLibrary::retire (std::shared_ptr<const SampleData> d)
{
    if (d == nullptr)
        return;

    retired.push_back ({ std::move (d), audioBlockCounter.load() });

    if (! isTimerRunning())
        startTimer (250);
}

void SampleLibrary::timerCallback()
{
    const auto now = audioBlockCounter.load();

    // Safe once the audio thread has begun two blocks after the removal: the
    // block that was in flight has finished, and the next one has already
    // dropped any voice that used the removed sample.
    retired.erase (std::remove_if (retired.begin(), retired.end(),
                                   [now] (const Retired& r) { return now >= r.epoch + 2; }),
                   retired.end());

    if (retired.empty())
        stopTimer();
}

// ---- undo / redo ------------------------------------------------------------------
SampleLibrary::Snapshot SampleLibrary::makeSnapshot() const
{
    Snapshot s;
    s.selected = selected.load();
    s.entries.reserve ((size_t) numLoaded);

    for (int i = 0; i < numLoaded; ++i)
    {
        Entry e;
        e.sample = slots[(size_t) i];
        e.sample->getRegion (e.start, e.end);
        e.delayMs = e.sample->delayMs.load();
        s.entries.push_back (std::move (e));
    }

    return s;
}

bool SampleLibrary::sameContent (const Snapshot& a, const Snapshot& b) noexcept
{
    if (a.entries.size() != b.entries.size())
        return false;

    for (size_t i = 0; i < a.entries.size(); ++i)
    {
        const auto &x = a.entries[i], &y = b.entries[i];

        if (x.sample != y.sample || x.start != y.start || x.end != y.end || std::abs (x.delayMs - y.delayMs) > 0.01f)
            return false;
    }

    return true;
}

void SampleLibrary::pushHistory (Snapshot before)
{
    perf::ScopedTimer timer (perfMon, perf::Id::HistoryOp);

    dragBefore.reset();

    if (sameContent (before, makeSnapshot()))
        return;                                   // nothing actually changed

    undoStack.push_back (std::move (before));
    redoStack.clear();
    trimHistory();
}

void SampleLibrary::trimHistory()
{
    while (undoStack.size() > kMaxHistorySteps)
        undoStack.pop_front();

    // Audio that only history still holds (removed files) must stay under a memory budget.
    while (! undoStack.empty())
    {
        std::vector<const SampleData*> seen;
        size_t bytes = 0;

        auto count = [&] (const Snapshot& s)
        {
            for (const auto& e : s.entries)
            {
                const SampleData* p = e.sample.get();
                bool loaded = false;

                for (int i = 0; i < numLoaded && ! loaded; ++i)
                    loaded = slots[(size_t) i].get() == p;

                if (! loaded && std::find (seen.begin(), seen.end(), p) == seen.end())
                {
                    seen.push_back (p);
                    bytes += p->getMemoryBytes();
                }
            }
        };

        for (const auto& s : undoStack) count (s);
        for (const auto& s : redoStack) count (s);

        if (bytes <= kMaxHistoryBytes)
            break;

        undoStack.pop_front();
    }
}

void SampleLibrary::clearHistory()
{
    undoStack.clear();
    redoStack.clear();
    dragBefore.reset();
}

// Makes `s` the library's content. Samples that leave are unpublished and retired (kept alive until
// the audio thread cannot touch them); samples that come back are published again.
void SampleLibrary::applySnapshot (const Snapshot& s)
{
    for (int i = 0; i < numLoaded; ++i)
    {
        const auto& cur = slots[(size_t) i];
        const bool stays = std::any_of (s.entries.begin(), s.entries.end(),
                                        [&] (const Entry& e) { return e.sample == cur; });
        if (! stays)
        {
            cur->removed.store (true, std::memory_order_release);
            retire (cur);
        }
    }

    for (size_t i = 0; i < s.entries.size(); ++i)
    {
        const auto& e = s.entries[i];
        e.sample->removed.store (false, std::memory_order_release);
        e.sample->setRegion (e.start, e.end);
        e.sample->delayMs.store (e.delayMs);
        slots[i] = e.sample;
        audioSlots[i].store (e.sample.get(), std::memory_order_release);
    }

    for (size_t i = s.entries.size(); i < (size_t) kMaxSamples; ++i)
    {
        slots[i] = nullptr;
        audioSlots[i].store (nullptr, std::memory_order_release);
    }

    numLoaded = (int) s.entries.size();
    selected.store (juce::jlimit (0, juce::jmax (0, numLoaded - 1), s.selected));
    statusMessage = {};
}

void SampleLibrary::undo()
{
    if (undoStack.empty())
        return;

    perf::ScopedTimer timer (perfMon, perf::Id::HistoryOp);

    auto current = makeSnapshot();
    auto target = std::move (undoStack.back());
    undoStack.pop_back();
    redoStack.push_back (std::move (current));
    applySnapshot (target);
    trimHistory();
    notify();
}

void SampleLibrary::redo()
{
    if (redoStack.empty())
        return;

    perf::ScopedTimer timer (perfMon, perf::Id::HistoryOp);

    auto current = makeSnapshot();
    auto target = std::move (redoStack.back());
    redoStack.pop_back();
    undoStack.push_back (std::move (current));
    applySnapshot (target);
    trimHistory();
    notify();
}
