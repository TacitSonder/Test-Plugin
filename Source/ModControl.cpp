#include "ModControl.h"
#include "UiTheme.h"

void ModeSwitch::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    const auto pill = juce::Rectangle<float> (36.0f, 16.0f).withCentre (b.getCentre());

    g.setFont (juce::FontOptions (12.0f));
    g.setColour (envelopeMode ? theme::text : theme::subtle);
    g.drawText ("ENV", b.withRight (pill.getX() - 4.0f), juce::Justification::centredRight, false);
    g.setColour (envelopeMode ? theme::subtle : theme::text);
    g.drawText ("KNOB", b.withLeft (pill.getRight() + 4.0f), juce::Justification::centredLeft, false);

    g.setColour (theme::background);
    g.fillRoundedRectangle (pill, 8.0f);
    g.setColour (theme::outline);
    g.drawRoundedRectangle (pill.reduced (0.5f), 8.0f, 1.0f);

    const float d = pill.getHeight() - 4.0f;                                    // thumb: left = ENV, right = KNOB
    g.setColour (accent);
    g.fillEllipse (envelopeMode ? pill.getX() + 2.0f : pill.getRight() - 2.0f - d, pill.getY() + 2.0f, d, d);
}

void ModeSwitch::mouseDown (const juce::MouseEvent&)
{
    envelopeMode = ! envelopeMode;
    repaint();

    if (onChange)
        onChange (envelopeMode);
}

// -----------------------------------------------------------------------------
ModControl::ModControl (const juce::String& t, ModParam& p, perf::Monitor& pm) : title (t), param (p), editor (pm)
{
    knob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    knob.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
    knob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    const bool isLevel = param.getKind() == ModParam::Kind::Level;
    const double lowest = (double) param.getMin();
    knob.textFromValueFunction = [isLevel, lowest] (double v)               // level in dB, "-inf" at the bottom
    {
        return isLevel ? (v <= lowest + 1.0e-3 ? juce::String ("-inf") : juce::String (v, 1) + " dB") : juce::String (v, 2);
    };

    if (param.getKind() == ModParam::Kind::Speed)            // logarithmic: x1 in the middle, x0.5 and x2 equally far from it
        knob.setNormalisableRange (juce::NormalisableRange<double> (
            (double) param.getMin(), (double) param.getMax(),
            [] (double s, double e, double n) { return s * std::pow (e / s, n); },
            [] (double s, double e, double v) { return std::log (v / s) / std::log (e / s); }));
    else
        knob.setRange ((double) param.getMin(), (double) param.getMax(), 0.1);      // dB in 0.1 steps

    knob.setDoubleClickReturnValue (true, (double) param.getDefault());
    knob.onValueChange = [this] { param.setKnob ((float) knob.getValue()); };
    addAndMakeVisible (knob);

    editor.onChanged = [this] (const mod::Envelope& e) { param.setEnvelope (e); };
    editor.defaultEnvelope = [this] { return param.getDefaultEnvelope(); };
    editor.yLabel = [this] (float n)
    {
        const float u = param.unitsFromNorm ((double) n);
        if (param.getKind() == ModParam::Kind::Speed)
            return "x" + juce::String (u, 2);
        return u <= param.getMin() + 1.0e-3f ? juce::String ("-inf") : juce::String (u, 1) + " dB";
    };
    addChildComponent (editor);

    modeSwitch.onChange = [this] (bool env) { param.setEnvelopeMode (env); applyMode (env); };
    addAndMakeVisible (modeSwitch);

    refresh();
}

void ModControl::refresh()
{
    knob.setValue ((double) param.getKnob(), juce::dontSendNotification);
    editor.setEnvelope (param.getEnvelope());
    modeSwitch.setEnvelopeMode (param.isEnvelopeMode());
    applyMode (param.isEnvelopeMode());
}

void ModControl::setAccent (juce::Colour c)
{
    knob.setColour (juce::Slider::rotarySliderFillColourId, c);
    editor.setAccent (c);
    modeSwitch.setAccent (c);
    knob.repaint();
}

void ModControl::applyMode (bool envelopeMode)
{
    knob.setVisible (! envelopeMode);
    editor.setVisible (envelopeMode);
}

void ModControl::paint (juce::Graphics& g)
{
    g.setColour (theme::subtle);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (title, titleArea, juce::Justification::centred, false);
}

void ModControl::resized()
{
    auto r = getLocalBounds().reduced (4, 0);
    titleArea = r.removeFromTop (20);
    modeSwitch.setBounds (r.removeFromBottom (22));
    knob.setBounds (r);
    editor.setBounds (r);
}
