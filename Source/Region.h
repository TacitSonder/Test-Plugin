#pragma once

#include <juce_core/juce_core.h>

// Pure helpers for "which part of the file plays" (frames, half-open [start, end)).
// Header-only and free of GUI/audio dependencies so they can be unit-tested and
// used from the audio thread.
namespace region
{
    constexpr int kMinFrames = 32;     // shortest selectable region

    // start in the low 32 bits, end in the high 32: one lock-free atomic holds both.
    inline juce::uint64 pack (int start, int end) noexcept
    {
        return (juce::uint64) (juce::uint32) start | ((juce::uint64) (juce::uint32) end << 32);
    }

    inline void unpack (juce::uint64 packed, int& start, int& end) noexcept
    {
        start = (int) (juce::uint32) (packed & 0xffffffffu);
        end   = (int) (juce::uint32) (packed >> 32);
    }

    // Makes (start, end) a valid region inside [0, numFrames], at least kMinFrames long.
    inline void sanitize (int& start, int& end, int numFrames) noexcept
    {
        numFrames = juce::jmax (0, numFrames);
        start = juce::jlimit (0, numFrames, start);
        end   = juce::jlimit (0, numFrames, end);

        if (end < start)
            std::swap (start, end);

        const int minLen = juce::jmin (kMinFrames, numFrames);

        if (end - start < minLen)
        {
            if (start + minLen <= numFrames) end = start + minLen;
            else                             { end = numFrames; start = numFrames - minLen; }
        }
    }

    inline bool isFull (int start, int end, int numFrames) noexcept { return start <= 0 && end >= numFrames; }

    inline float frameToX (int frame, int numFrames, float left, float width) noexcept
    {
        return numFrames > 0 ? left + width * (float) frame / (float) numFrames : left;
    }

    inline int xToFrame (float x, int numFrames, float left, float width) noexcept
    {
        if (width <= 0.0f || numFrames <= 0)
            return 0;

        const double t = juce::jlimit (0.0, 1.0, (double) (x - left) / (double) width);
        return (int) juce::roundToInt (t * (double) numFrames);
    }

    enum class Edge { None, Start, End };

    // Which region edge (if any) is within `tolerance` pixels of x. If both are, the nearer wins.
    inline Edge hitEdge (float x, float xStart, float xEnd, float tolerance) noexcept
    {
        const float ds = std::abs (x - xStart), de = std::abs (x - xEnd);

        if (ds > tolerance && de > tolerance)
            return Edge::None;

        if (std::abs (ds - de) < 1.0e-4f)                     // equidistant (e.g. empty region)
            return x < xStart ? Edge::Start : Edge::End;

        return ds < de ? Edge::Start : Edge::End;
    }
}
