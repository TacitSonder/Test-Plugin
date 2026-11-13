#pragma once

// How many layers the plugin has and what they are called. Everything that is per layer
// (sample bank, speed, level, tab, colour, timeline row) is sized from this, so adding a
// layer later is a one-line change here plus a colour in UiTheme.h.
namespace layercfg
{
    constexpr int kCount = 2;
    inline constexpr const char* kNames[kCount] = { "ONSET", "BODY" };
}
