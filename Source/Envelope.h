#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <cmath>

// Modulation envelope shared by the UI (editing) and the audio thread (playback).
// Header-only and free of GUI code so it can be unit-tested and benchmarked.
namespace mod
{
    constexpr int kMaxPoints = 8;
    constexpr int kTableSize = 256;

    struct Point { float x = 0.0f, y = 0.0f; };       // x: position in the sound 0..1, y: normalised value 0..1

    // A few breakpoints, ordered by x (equal x = a vertical step). The first point sits at x = 0
    // and the last at x = 1; the points in between move freely between their neighbours.
    struct Envelope
    {
        int count = 0;
        std::array<Point, kMaxPoints> pts {};

        static Envelope flat (float y)                  // 5 evenly spaced points at one height
        {
            Envelope e;
            e.count = 5;
            for (int i = 0; i < 5; ++i)
                e.pts[(size_t) i] = { 0.25f * (float) i, y };
            return e;
        }

        static Envelope gate()                          // 0 -> 1 instantly, hold, 1 -> 0 at the very end
        {
            Envelope e;
            e.count = 5;
            e.pts = { { { 0.0f, 0.0f }, { 0.0f, 1.0f }, { 0.5f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 0.0f } } };
            return e;
        }

        float valueAt (float x) const noexcept
        {
            if (count <= 0)
                return 0.0f;

            int k = 0;
            while (k + 1 < count && pts[(size_t) k + 1].x <= x)
                ++k;                                    // last point at or before x

            if (k + 1 >= count)
                return pts[(size_t) k].y;

            const auto &a = pts[(size_t) k], &b = pts[(size_t) k + 1];
            return a.y + (b.y - a.y) * (x - a.x) / (b.x - a.x);     // b.x > x >= a.x
        }

        int addPoint (float x, float y) noexcept        // returns the new index, or -1 when full
        {
            if (count >= kMaxPoints)
                return -1;

            x = juce::jlimit (0.0f, 1.0f, x);
            int i = count;
            while (i > 0 && pts[(size_t) i - 1].x > x)
            {
                pts[(size_t) i] = pts[(size_t) i - 1];
                --i;
            }
            pts[(size_t) i] = { x, juce::jlimit (0.0f, 1.0f, y) };
            ++count;
            return i;
        }

        void removePoint (int i) noexcept
        {
            for (int k = i; k + 1 < count; ++k)
                pts[(size_t) k] = pts[(size_t) k + 1];
            --count;
        }

        void normalize() noexcept                       // after loading untrusted data
        {
            if (count < 2 || count > kMaxPoints)
                *this = flat (0.5f);

            float prev = 0.0f;
            for (int i = 0; i < count; ++i)
            {
                auto& p = pts[(size_t) i];
                p.y = juce::jlimit (0.0f, 1.0f, p.y);
                p.x = juce::jlimit (prev, 1.0f, p.x);
                prev = p.x;
            }
            pts[0].x = 0.0f;
            pts[(size_t) count - 1].x = 1.0f;
        }
    };

    // The envelope sampled at kTableSize + 1 positions, in real parameter values.
    // The audio thread only ever does a linear lookup in this.
    struct EnvTable
    {
        std::array<float, kTableSize + 1> v {};

        float at (double x) const noexcept
        {
            const double p = juce::jlimit (0.0, 1.0, x) * (double) kTableSize;
            const int i = juce::jmin ((int) p, kTableSize - 1);
            const float f = (float) (p - (double) i);
            return v[(size_t) i] + f * (v[(size_t) i + 1] - v[(size_t) i]);
        }
    };

    // Lock-free hand-over of a value from ONE writer thread to ONE reader thread.
    // Each side owns a private buffer; a third one in the middle is swapped atomically, so
    // the reader never sees a half-written value and neither side ever waits.
    template <typename T>
    class TripleBuffer
    {
    public:
        void fillAll (const T& value)                   { for (auto& b : buffers) b = value; }

        // writer: fill writeBuffer(), then publish()
        T& writeBuffer() noexcept                       { return buffers[(size_t) writeIdx]; }
        void publish() noexcept                         { writeIdx = middle.exchange (writeIdx | kNew, std::memory_order_acq_rel) & 3; }

        // reader: newest published value (the reference stays valid until the next read())
        const T& read() noexcept
        {
            if (middle.load (std::memory_order_acquire) & kNew)
                readIdx = middle.exchange (readIdx, std::memory_order_acq_rel) & 3;
            return buffers[(size_t) readIdx];
        }

    private:
        static constexpr int kNew = 4;
        std::array<T, 3> buffers;
        int writeIdx = 0, readIdx = 1;                  // each owned by one thread only
        std::atomic<int> middle { 2 };
    };
}
