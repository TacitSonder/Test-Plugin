#include "LayerParams.h"

ModParam::ModParam (Kind k, perf::Monitor& pm) : kind (k), perfMon (pm)
{
    setKnob (getDefault());
    table.fillAll ({});
    setEnvelope (getDefaultEnvelope());
}

float ModParam::unitsFromNorm (double p) const noexcept
{
    p = juce::jlimit (0.0, 1.0, p);
    return kind == Kind::Speed ? (float) (0.25 * std::pow (16.0, p))     // speed: logarithmic, x1 in the middle
                               : (float) (-60.0 + 60.0 * p);             // level: linear in dB
}

double ModParam::toNorm (float units) const noexcept
{
    return kind == Kind::Speed ? std::log ((double) units / 0.25) / std::log (16.0)
                               : ((double) units + 60.0) / 60.0;
}

float ModParam::toAudio (float units) const noexcept
{
    if (kind == Kind::Speed)
        return units;

    return units <= getMin() + 1.0e-4f ? 0.0f : std::pow (10.0f, units * 0.05f);     // bottom of the range = silence
}

mod::Envelope ModParam::getDefaultEnvelope() const
{
    return kind == Kind::Speed ? mod::Envelope::flat ((float) toNorm (1.0f)) : mod::Envelope::gate();
}

mod::Envelope ModParam::getEnvelope() const
{
    const juce::ScopedLock sl (lock);
    return envelope;
}

void ModParam::setEnvelope (const mod::Envelope& e)
{
    perf::ScopedTimer timer (perfMon, perf::Id::ModTable);

    const juce::ScopedLock sl (lock);                   // one writer at a time
    envelope = e;

    auto& t = table.writeBuffer();
    for (int i = 0; i <= mod::kTableSize; ++i)
        t.v[(size_t) i] = fromNorm ((double) envelope.valueAt ((float) i / (float) mod::kTableSize));

    table.publish();
}

void LayerParams::saveTo (juce::XmlElement& layer) const
{

    auto put = [&layer] (const char* tag, const ModParam& p)
    {
        auto* m = layer.createNewChildElement (tag);
        m->setAttribute ("knob", (double) p.getKnob());
        m->setAttribute ("env", p.isEnvelopeMode() ? 1 : 0);

        const auto e = p.getEnvelope();
        for (int i = 0; i < e.count; ++i)
        {
            auto* pt = m->createNewChildElement ("P");
            pt->setAttribute ("x", (double) e.pts[(size_t) i].x);
            pt->setAttribute ("y", (double) e.pts[(size_t) i].y);
        }
    };

    put ("Speed", speed);
    put ("Level", level);
}

void LayerParams::loadFrom (const juce::XmlElement& layer)
{
    auto get = [&layer] (const char* tag, ModParam& p)
    {
        const auto* m = layer.getChildByName (tag);
        if (m == nullptr)
            return;

        p.setKnob ((float) m->getDoubleAttribute ("knob", (double) p.getDefault()));
        p.setEnvelopeMode (m->getIntAttribute ("env", 0) != 0);

        mod::Envelope e;
        for (auto* pt : m->getChildIterator())
            if (pt->hasTagName ("P") && e.count < mod::kMaxPoints)
                e.pts[(size_t) e.count++] = { (float) pt->getDoubleAttribute ("x"), (float) pt->getDoubleAttribute ("y") };

        e.normalize();
        p.setEnvelope (e);
    };

    get ("Speed", speed);
    get ("Level", level);
    sendChangeMessage();
}
