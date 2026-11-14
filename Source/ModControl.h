#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "EnvelopeEditor.h"
#include "LayerParams.h"

#include <functional>

// "ENV  (o)  KNOB" toggle.
class ModeSwitch : public juce::Component
{
public:
    std::function<void (bool envelopeMode)> onChange;

    void setEnvelopeMode (bool b)         { if (b != envelopeMode) { envelopeMode = b; repaint(); } }
    void setAccent (juce::Colour c)       { accent = c; repaint(); }
    bool isEnvelopeMode() const noexcept  { return envelopeMode; }

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    bool envelopeMode = false;
    juce::Colour accent { 0xff4fc3f7 };
};

// One modulatable parameter of a layer: a title, a knob that can be swapped for an envelope
// editor, and the ENV/KNOB switch. Bound to a ModParam, so every future layer reuses it as is.
class ModControl : public juce::Component
{
public:
    ModControl (const juce::String& title, ModParam&, perf::Monitor&);

    void refresh();                                   // re-read everything from the ModParam
    void setAccent (juce::Colour c);                  // the layer's colour: knob arc, envelope, switch

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void applyMode (bool envelopeMode);

    juce::String title;
    ModParam& param;
    juce::Slider knob;
    EnvelopeEditor editor;
    ModeSwitch modeSwitch;
    juce::Rectangle<int> titleArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModControl)
};
