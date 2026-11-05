#include "PluginProcessor.h"
#include "PluginEditor.h"

HelloProcessor::HelloProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
}

void HelloProcessor::prepareToPlay (double, int) {}
void HelloProcessor::releaseResources() {}

bool HelloProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();

    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return out == layouts.getMainInputChannelSet();
}

void HelloProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // Clear any output channels that have no matching input.
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    // Pass-through: the buffer already holds the input, so leave it untouched.
}

juce::AudioProcessorEditor* HelloProcessor::createEditor()
{
    return new HelloEditor (*this);
}

// Required by JUCE: creates the plugin instance.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new HelloProcessor();
}
