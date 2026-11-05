#pragma once

#include "PluginProcessor.h"

class HelloEditor : public juce::AudioProcessorEditor
{
public:
    explicit HelloEditor (HelloProcessor&);
    ~HelloEditor() override = default;

    void paint (juce::Graphics&) override;
    void resized() override {}

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HelloEditor)
};
