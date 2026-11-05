#include "PluginProcessor.h"
#include "PluginEditor.h"

SamplerProcessor::SamplerProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      library (perfMonitor),
      engine (library, perfMonitor)
{
    library.addListener (this);
}

SamplerProcessor::~SamplerProcessor()
{
    library.removeListener (this);
}

void SamplerProcessor::prepareToPlay (double sampleRate, int)
{
    perfMonitor.prepare (sampleRate);
    engine.prepare (sampleRate);
}

bool SamplerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainInputChannelSet().isDisabled();
}

void SamplerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    const auto start = perf::Monitor::now();
    engine.process (buffer, midi);
    perfMonitor.recordAudioBlock (perf::Monitor::now() - start, buffer.getNumSamples(), engine.getNumActiveVoices());
}

juce::AudioProcessorEditor* SamplerProcessor::createEditor()
{
    return new SamplerEditor (*this);
}

// ---- project state: which files are loaded, which is selected, window size ----
void SamplerProcessor::sampleLibraryChanged()
{
    const juce::ScopedLock sl (stateLock);
    statePaths = library.getPaths();
    stateSelected = library.getSelected();
}

void SamplerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::XmlElement xml ("SampleDropState");
    xml.setAttribute ("version", 1);
    xml.setAttribute ("width", editorWidth.load());
    xml.setAttribute ("height", editorHeight.load());

    {
        const juce::ScopedLock sl (stateLock);
        xml.setAttribute ("selected", stateSelected);

        for (const auto& p : statePaths)
            xml.createNewChildElement ("Sample")->setAttribute ("path", p);
    }

    copyXmlToBinary (xml, destData);
}

void SamplerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr || ! xml->hasTagName ("SampleDropState"))
        return;

    editorWidth.store (xml->getIntAttribute ("width", editorWidth.load()));
    editorHeight.store (xml->getIntAttribute ("height", editorHeight.load()));

    juce::StringArray paths;
    for (auto* child : xml->getChildIterator())
        if (child->hasTagName ("Sample"))
            paths.add (child->getStringAttribute ("path"));

    const int selected = xml->getIntAttribute ("selected", 0);

    // The library lives on the message thread; hosts may call this from elsewhere.
    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        library.restore (paths, selected);
    }
    else
    {
        juce::MessageManager::callAsync ([weak = juce::WeakReference<SampleLibrary> (&library), paths, selected]
        {
            if (auto* lib = weak.get())
                lib->restore (paths, selected);
        });
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SamplerProcessor();
}
