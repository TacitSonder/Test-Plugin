#include "PluginProcessor.h"
#include "PluginEditor.h"

SamplerProcessor::SamplerProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    for (auto& l : layers)
    {
        l = std::make_unique<Layer> (perfMonitor);
        l->library.addListener (this);
    }
}

SamplerProcessor::~SamplerProcessor()
{
    for (auto& l : layers)
        l->library.removeListener (this);
}

void SamplerProcessor::prepareToPlay (double sampleRate, int)
{
    perfMonitor.prepare (sampleRate);

    for (auto& l : layers)
        l->engine.prepare (sampleRate);
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

    // The first layer clears the buffer, every other layer adds its sound on top.
    int voices = 0;
    bool first = true;

    for (auto& l : layers)
    {
        l->engine.process (buffer, midi, first);
        voices += l->engine.getNumActiveVoices();
        first = false;
    }

    perfMonitor.recordAudioBlock (perf::Monitor::now() - start, buffer.getNumSamples(), voices);
}

void SamplerProcessor::triggerFromGui() noexcept
{
    for (auto& l : layers)
        l->engine.requestTrigger();
}

juce::AudioProcessorEditor* SamplerProcessor::createEditor()
{
    return new SamplerEditor (*this);
}

// ---- project state: files + selection + speed/level per layer, window size, zoom, active tab ----
void SamplerProcessor::sampleLibraryChanged()
{
    const juce::ScopedLock sl (stateLock);

    for (size_t i = 0; i < layers.size(); ++i)
    {
        stateFiles[i] = layers[i]->library.getState();
        stateSelected[i] = layers[i]->library.getSelected();
    }
}

void SamplerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::XmlElement xml ("SampleDropState");
    xml.setAttribute ("version", 2);
    xml.setAttribute ("width", editorWidth.load());
    xml.setAttribute ("height", editorHeight.load());
    xml.setAttribute ("scale", scaleMs.load());
    xml.setAttribute ("tab", activeTab.load());

    {
        const juce::ScopedLock sl (stateLock);

        for (size_t i = 0; i < layers.size(); ++i)
        {
            auto* layer = xml.createNewChildElement ("Layer");
            layer->setAttribute ("index", (int) i);
            layer->setAttribute ("selected", stateSelected[i]);

            for (const auto& f : stateFiles[i])
            {
                auto* e = layer->createNewChildElement ("Sample");
                e->setAttribute ("path", f.path);
                e->setAttribute ("start", f.start);
                e->setAttribute ("end", f.end);
                e->setAttribute ("delay", (double) f.delayMs);
            }
        }
    }

    for (size_t i = 0; i < layers.size(); ++i)                    // parameters have their own lock
        if (auto* layer = xml.getChildElement ((int) i))
            layers[i]->params.saveTo (*layer);

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
    activeTab.store (juce::jlimit (0, layercfg::kCount - 1, xml->getIntAttribute ("tab", 0)));

    std::vector<LayerRestore> restore (layers.size());

    for (auto* layer : xml->getChildIterator())
    {
        const int index = layer->getIntAttribute ("index", -1);

        if (! layer->hasTagName ("Layer") || index < 0 || index >= layercfg::kCount)
            continue;

        auto& r = restore[(size_t) index];
        r.selected = layer->getIntAttribute ("selected", 0);
        r.xml = std::make_shared<juce::XmlElement> (*layer);

        for (auto* child : layer->getChildIterator())
            if (child->hasTagName ("Sample"))
                r.files.push_back ({ child->getStringAttribute ("path"),
                                     child->getIntAttribute ("start", -1),
                                     child->getIntAttribute ("end", -1),
                                     (float) child->getDoubleAttribute ("delay", 0.0) });
    }

    // The libraries and the parameter editors live on the message thread; hosts may call this from elsewhere.
    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        restoreOnMessageThread (restore);
    }
    else
    {
        juce::MessageManager::callAsync ([weak = juce::WeakReference<SamplerProcessor> (this), restore]
        {
            if (auto* p = weak.get())
                p->restoreOnMessageThread (restore);
        });
    }
}

void SamplerProcessor::restoreOnMessageThread (const std::vector<LayerRestore>& restore)
{
    for (size_t i = 0; i < layers.size() && i < restore.size(); ++i)
    {
        layers[i]->library.restore (restore[i].files, restore[i].selected);

        if (restore[i].xml != nullptr)
            layers[i]->params.loadFrom (*restore[i].xml);
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SamplerProcessor();
}
