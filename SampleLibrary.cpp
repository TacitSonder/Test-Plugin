#include "SampleLibrary.h"

namespace
{
    constexpr int    kReadChunkFrames = 1 << 16;   // decode in chunks so a cancel is noticed quickly
    constexpr double kMaxFileSeconds  = 600.0;     // guards against accidentally dropping huge files

    void buildPeaks (SampleData& d)
    {
        const int n  = d.numFrames;
        const int nb = juce::jmin (SampleData::kMaxPeakBuckets, n);
        d.numPeakBuckets = nb;
        d.peaks.assign ((size_t) nb * 2, 0.0f);

        const float* l = d.audio.getReadPointer (0);
        const float* r = d.numChannels > 1 ? d.audio.getReadPointer (1) : nullptr;

        for (int i = 0; i < nb; ++i)
        {
            const int s = (int) ((juce::int64) i * n / nb);
            const int e = (int) ((juce::int64) (i + 1) * n / nb);   // e > s because nb <= n

            auto range = juce::FloatVectorOperations::findMinAndMax (l + s, e - s);
            float mn = range.getStart(), mx = range.getEnd();

            if (r != nullptr)
            {
                auto rr = juce::FloatVectorOperations::findMinAndMax (r + s, e - s);
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
    int queued = 0, unsupported = 0, ignored = 0;

    for (const auto& p : paths)
    {
        const juce::File f (p);

        if (! isSupportedFile (f))
        {
            ++unsupported;
            continue;
        }

        if (numLoaded + (int) pending.size() >= kMaxSamples)
        {
            ++ignored;
            continue;
        }

        auto task = std::make_shared<LoadTask>();
        task->file = f;
        pending.push_back (task);
        getPool().addJob ([this, task] { runLoad (*task); });
        ++queued;
    }

    if (ignored > 0)
        statusMessage = plural (ignored, "file") + " ignored: the limit is " + juce::String (kMaxSamples)
                        + " files (use Clear to start over)";

    if (unsupported > 0)
    {
        if (statusMessage.isNotEmpty())
            statusMessage << "; ";
        statusMessage << plural (unsupported, "file") << " skipped (not a supported audio file)";
    }

    notify();
    return queued;
}

void SampleLibrary::restore (const juce::StringArray& paths, int selectedIndex)
{
    clear();
    addFiles (paths);
    selected.store (juce::jlimit (0, kMaxSamples - 1, selectedIndex));
}

void SampleLibrary::clear()
{
    for (auto& t : pending)
        t->cancelled.store (true);
    pending.clear();

    // 1) unpublish from the audio thread, 2) retire the memory
    for (auto& a : audioSlots)
        a.store (nullptr);

    for (int i = 0; i < numLoaded; ++i)
        retire (std::move (slots[(size_t) i]));

    numLoaded = 0;
    selected.store (0);
    statusMessage = {};
    notify();
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

juce::StringArray SampleLibrary::getPaths() const
{
    juce::StringArray paths;

    for (int i = 0; i < numLoaded; ++i)
        paths.add (slots[(size_t) i]->path);

    for (auto& t : pending)
        paths.add (t->file.getFullPathName());

    return paths;
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
            audioSlots[slot].store (slots[slot].get(), std::memory_order_release);
        }
        else if (! task->cancelled.load())
        {
            if (statusMessage.isNotEmpty())
                statusMessage << "; ";
            statusMessage << "could not load " << task->file.getFileName() << " (" << task->error << ")";
        }
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
