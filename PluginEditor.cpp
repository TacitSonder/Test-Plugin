#include "PluginEditor.h"
#include "UiTheme.h"

SamplerLookAndFeel::SamplerLookAndFeel()
{
    setColourScheme (getDarkColourScheme());

    setColour (juce::ComboBox::backgroundColourId, theme::panel);
    setColour (juce::ComboBox::outlineColourId, theme::outline);
    setColour (juce::ComboBox::textColourId, theme::text);
    setColour (juce::ComboBox::arrowColourId, theme::subtle);
    setColour (juce::PopupMenu::backgroundColourId, theme::panel);
    setColour (juce::PopupMenu::textColourId, theme::text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, theme::accent.withAlpha (0.35f));
    setColour (juce::PopupMenu::highlightedTextColourId, theme::text);
    setColour (juce::TextButton::buttonColourId, theme::panel);
    setColour (juce::TextButton::buttonOnColourId, theme::accent.withAlpha (0.45f));
    setColour (juce::TextButton::textColourOffId, theme::text);
    setColour (juce::TextButton::textColourOnId, theme::text);
}

// -----------------------------------------------------------------------------
SamplerEditor::SamplerEditor (SamplerProcessor& p)
    : AudioProcessorEditor (&p),
      processor (p),
      library (p.getLibrary()),
      perfMon (p.getPerf()),
      waveform (p.getPerf()),
      overlay (p.getPerf(), p.getLibrary())
{
    setLookAndFeel (&lookAndFeel);
    setOpaque (true);

    fileBox.setTextWhenNothingSelected ("Drop audio files here");
    fileBox.setTextWhenNoChoicesAvailable ("Drop audio files here");
    fileBox.onChange = [this] { library.setSelected (fileBox.getSelectedId() - 1); };
    addAndMakeVisible (fileBox);

    clearButton.onClick = [this] { library.clear(); };
    addAndMakeVisible (clearButton);

    addAndMakeVisible (waveform);

#if SAMPLER_PERF_ENABLED
    perfButton.setComponentID ("perfButton");
    perfButton.setClickingTogglesState (true);
    perfButton.onClick = [this]
    {
        overlay.setActive (perfButton.getToggleState());
        layoutOverlay();
    };
    addAndMakeVisible (perfButton);
    addChildComponent (overlay);   // added last so it paints on top of the waveform
#endif

    setResizable (true, true);
    setResizeLimits (480, 340, 2400, 1400);
    setSize (processor.getEditorWidth(), processor.getEditorHeight());

    library.addListener (this);
    refreshFromLibrary();
}

SamplerEditor::~SamplerEditor()
{
    library.removeListener (this);
    setLookAndFeel (nullptr);
}

// ---- layout -----------------------------------------------------------------
void SamplerEditor::resized()
{
    perf::ScopedTimer timer (perfMon, perf::Id::EditorResize);

    processor.setEditorSize (getWidth(), getHeight());

    const int w = getWidth(), h = getHeight();
    const int pad     = juce::jlimit (8, 18, juce::jmin (w, h) / 30);
    const int gap     = juce::jmax (4, pad / 2);
    const int barH    = juce::jlimit (26, 40, h / 11);
    const int statusH = juce::jlimit (18, 26, h / 16);
    const int buttonW = juce::jlimit (56, 90, w / 9);

    auto r = getLocalBounds().reduced (pad);

    auto bar = r.removeFromTop (barH);
#if SAMPLER_PERF_ENABLED
    perfButton.setBounds (bar.removeFromRight (buttonW));
    bar.removeFromRight (gap);
#endif
    clearButton.setBounds (bar.removeFromRight (buttonW));
    bar.removeFromRight (gap);
    fileBox.setBounds (bar);

    r.removeFromTop (gap);
    statusArea = r.removeFromBottom (statusH);
    r.removeFromBottom (gap / 2);
    waveform.setBounds (r);

    layoutOverlay();
}

void SamplerEditor::layoutOverlay()
{
    if (! overlay.isVisible())
        return;

    const auto wb = waveform.getBounds();
    const int ow = juce::jmin (wb.getWidth() - 16, PerfOverlay::kPreferredWidth);
    const int oh = juce::jmin (wb.getHeight() - 16, overlay.getPreferredHeight());
    overlay.setBounds (wb.getRight() - ow - 8, wb.getY() + 8, juce::jmax (0, ow), juce::jmax (0, oh));
}

void SamplerEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);

    g.setFont (juce::FontOptions (juce::jlimit (12.0f, 15.0f, (float) statusArea.getHeight() * 0.62f)));

    auto area = statusArea;

    if (area.getWidth() > 640)
    {
        g.setColour (theme::subtle);
        g.drawText ("Any MIDI note plays the selected file", area.removeFromRight (260),
                    juce::Justification::centredRight, true);
    }

    g.setColour (statusIsWarning ? theme::warning : theme::subtle);
    g.drawText (statusText, area, juce::Justification::centredLeft, true);
}

// ---- library -> UI ----------------------------------------------------------
void SamplerEditor::sampleLibraryChanged()
{
    refreshFromLibrary();
}

void SamplerEditor::refreshFromLibrary()
{
    const int n = library.getNumLoaded();
    const int sel = library.getSelected();

    fileBox.clear (juce::dontSendNotification);
    for (int i = 0; i < n; ++i)
        fileBox.addItem (juce::String (i + 1) + ":  " + library.getSample (i)->name, i + 1);

    if (n > 0)
        fileBox.setSelectedId (sel + 1, juce::dontSendNotification);

    fileBox.setEnabled (n > 0);
    clearButton.setEnabled (n > 0 || library.getNumPending() > 0);
    waveform.setSample (library.getSample (sel));

    statusText = juce::String (n) + " / " + juce::String (SampleLibrary::kMaxSamples) + " files loaded";

    if (library.getNumPending() > 0)
        statusText << "   |   loading " << library.getNumPending() << "...";

    statusIsWarning = library.getStatusMessage().isNotEmpty();
    if (statusIsWarning)
        statusText << "   |   " << library.getStatusMessage();

    repaint (statusArea);
}

// ---- drag and drop ----------------------------------------------------------
bool SamplerEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    if (files == lastDragFiles)
        return lastDragResult;

    lastDragFiles = files;
    lastDragResult = false;

    for (const auto& f : files)
        if (library.isSupportedFile (juce::File (f)))
        {
            lastDragResult = true;
            break;
        }

    return lastDragResult;
}

void SamplerEditor::fileDragEnter (const juce::StringArray&, int, int)
{
    waveform.setDropHighlight (true);
}

void SamplerEditor::fileDragExit (const juce::StringArray&)
{
    waveform.setDropHighlight (false);
    lastDragFiles.clear();
}

void SamplerEditor::filesDropped (const juce::StringArray& files, int, int)
{
    waveform.setDropHighlight (false);
    lastDragFiles.clear();
    library.addFiles (files);
}
