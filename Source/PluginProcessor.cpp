#include "PluginProcessor.h"
#include "PluginEditor.h"

SamplerProcessor::SamplerProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      library (perfMonitor),
      layerParams (perfMonitor),
      engine (library, layerParams, perfMonitor)
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
    stateFiles = library.getState();
    stateSelected = library.getSelected();
}

void SamplerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::XmlElement xml ("SampleDropState");
    xml.setAttribute ("version", 1);
    xml.setAttribute ("width", editorWidth.load());
    xml.setAttribute ("height", editorHeight.load());
    xml.setAttribute ("scale", scaleMs.load());

    {
        const juce::ScopedLock sl (stateLock);
        xml.setAttribute ("selected", stateSelected);

        for (const auto& f : stateFiles)
        {
            auto* e = xml.createNewChildElement ("Sample");
            e->setAttribute ("path", f.path);
            e->setAttribute ("start", f.start);
            e->setAttribute ("end", f.end);
            e->setAttribute ("delay", (double) f.delayMs);
        }
    }

    layerParams.saveTo (xml);
    copyXmlToBinary (xml, destData);
}

void SamplerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr || ! xml->hasTagName ("SampleDropState"))
        return;

    editorWidth.store (xml->getIntAttribute ("width", editorWidth.load()));
    editorHeight.store (xml->getIntAttribute ("height", editorHeight.load()));
    scaleMs.store (juce::jlimit (100, 5000, xml->getIntAttribute ("scale", scaleMs.load())));

    std::vector<SampleLibrary::FileState> files;
    for (auto* child : xml->getChildIterator())
        if (child->hasTagName ("Sample"))
            files.push_back ({ child->getStringAttribute ("path"),
                               child->getIntAttribute ("start", -1),
                               child->getIntAttribute ("end", -1),
                               (float) child->getDoubleAttribute ("delay", 0.0) });

    const int selected = xml->getIntAttribute ("selected", 0);

    std::shared_ptr<juce::XmlElement> layerXml;
    if (auto* l = xml->getChildByName ("Layer"))
        layerXml = std::make_shared<juce::XmlElement> (*l);

    // The library and the parameter editors live on the message thread; hosts may call this from elsewhere.
    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        restoreOnMessageThread (files, selected, layerXml.get());
    }
    else
    {
        juce::MessageManager::callAsync ([weak = juce::WeakReference<SamplerProcessor> (this), files, selected, layerXml]
        {
            if (auto* p = weak.get())
                p->restoreOnMessageThread (files, selected, layerXml.get());
        });
    }
}

void SamplerProcessor::restoreOnMessageThread (const std::vector<SampleLibrary::FileState>& files, int selected,
                                               const juce::XmlElement* layerXml)
{
    library.restore (files, selected);

    if (layerXml != nullptr)
        layerParams.loadFrom (*layerXml);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SamplerProcessor();
}
