#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace theme
{
    inline const juce::Colour background { 0xff14161b };
    inline const juce::Colour panel      { 0xff1c1f26 };
    inline const juce::Colour outline    { 0xff343a46 };
    inline const juce::Colour accent     { 0xff4fc3f7 };
    inline const juce::Colour text       { 0xffe6e8ec };
    inline const juce::Colour subtle     { 0xff8b93a1 };
    inline const juce::Colour warning    { 0xffffb74d };
    inline const juce::Colour danger     { 0xffff6b6b };

    // Colour of each layer's tab, controls and timeline waveform (as in the reference design).
    inline juce::Colour layerColour (int layer)
    {
        static const juce::Colour colours[] = { juce::Colour (0xffffb938), juce::Colour (0xff5b8def),
                                                juce::Colour (0xffe5534b), juce::Colour (0xff6cc24a) };
        return colours[layer % 4];
    }

    inline juce::Colour textOn (juce::Colour fill)      // black or white, whichever reads better
    {
        return fill.getPerceivedBrightness() > 0.55f ? juce::Colours::black.withAlpha (0.8f) : juce::Colours::white;
    }
}
