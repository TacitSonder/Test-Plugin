#include "PluginEditor.h"

HelloEditor::HelloEditor (HelloProcessor& p)
    : AudioProcessorEditor (&p)
{
    setSize (480, 220);
}

void HelloEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1e1e2e));

    auto area = getLocalBounds().reduced (20);

    g.setColour (juce::Colours::white);
    g.setFont (30.0f);
    g.drawText ("Hello from my first VST3!", area.removeFromTop (area.getHeight() / 2),
                juce::Justification::centredBottom);

    // Shows when this exact build was compiled, so you can confirm
    // that a rebuilt plugin is really the one loaded in your DAW.
    g.setColour (juce::Colours::lightgrey);
    g.setFont (15.0f);
    g.drawText (juce::String ("Built: ") + __DATE__ + " " + __TIME__, area,
                juce::Justification::centredTop);
}
