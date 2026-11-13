#include "LayerPage.h"
#include "FileMenuItem.h"
#include "UiTheme.h"

LayerPage::LayerPage (SamplerProcessor& p, int layerIndex)
    : processor (p),
      library (p.getLibrary (layerIndex)),
      perfMon (p.getPerf()),
      index (layerIndex),
      waveform (p.getPerf()),
      selection (p.getPerf(), waveform),
      speedControl ("SPEED", p.getLayer (layerIndex).speed, p.getPerf()),
      levelControl ("LEVEL", p.getLayer (layerIndex).level, p.getPerf())
{
    const auto colour = theme::layerColour (index);
    waveform.setAccent (colour);
    selection.setAccent (colour);
    speedControl.setAccent (colour);
    levelControl.setAccent (colour);

    fileBox.setTextWhenNothingSelected ("Drop audio files here");
    fileBox.setTextWhenNoChoicesAvailable ("Drop audio files here");
    fileBox.onChange = [this] { library.setSelected (fileBox.getSelectedId() - 1); };
    addAndMakeVisible (fileBox);

    undoButton.setTooltip ("Undo (Ctrl+Z): import, clear, remove, selection and delay edits of this layer");
    undoButton.onClick = [this] { library.undo(); };
    addAndMakeVisible (undoButton);

    redoButton.setTooltip ("Redo (Ctrl+Y)");
    redoButton.onClick = [this] { library.redo(); };
    addAndMakeVisible (redoButton);

    clearButton.onClick = [this] { library.clear(); };
    addAndMakeVisible (clearButton);

    addAndMakeVisible (waveform);

    selection.onCommit = [this] (int start, int end) { library.setRegion (library.getSelected(), start, end); };
    addAndMakeVisible (selection);                       // above the waveform

    addAndMakeVisible (speedControl);
    addAndMakeVisible (levelControl);

    processor.getLayer (index).addChangeListener (this);
    library.addListener (this);
    refreshFromLibrary();
}

LayerPage::~LayerPage()
{
    library.removeListener (this);
    processor.getLayer (index).removeChangeListener (this);
}

void LayerPage::resized()
{
    const int w = getWidth(), h = getHeight();
    const int gap = juce::jmax (4, juce::jmin (w, h) / 60);
    const int barH = juce::jlimit (26, 40, h / 8);
    const int buttonW = juce::jlimit (56, 90, w / 9);

    auto r = getLocalBounds();
    auto bar = r.removeFromTop (barH);
    clearButton.setBounds (bar.removeFromRight (buttonW));
    bar.removeFromRight (gap);
    redoButton.setBounds (bar.removeFromRight (buttonW * 4 / 5));
    bar.removeFromRight (gap);
    undoButton.setBounds (bar.removeFromRight (buttonW * 4 / 5));
    bar.removeFromRight (gap);
    fileBox.setBounds (bar);

    r.removeFromTop (gap);

    // SPEED and LEVEL sit beside the waveform, like one row of a layer
    auto controls = r.removeFromRight (juce::jlimit (240, 340, w * 2 / 5));
    r.removeFromRight (gap);
    speedControl.setBounds (controls.removeFromLeft (controls.getWidth() / 2));
    levelControl.setBounds (controls);

    waveform.setBounds (r);
    selection.setBounds (r);
}

void LayerPage::changeListenerCallback (juce::ChangeBroadcaster*)
{
    speedControl.refresh();
    levelControl.refresh();
}

void LayerPage::sampleLibraryChanged()
{
    refreshFromLibrary();
}

void LayerPage::refreshFromLibrary()
{
    const int n = library.getNumLoaded();
    const int sel = library.getSelected();

    // Rebuild the dropdown only when the list of names changed (not on every region edit).
    juce::StringArray names;
    for (int i = 0; i < n; ++i)
        names.add (library.getSample (i)->name);

    if (names != menuNames)
    {
        perf::ScopedTimer timer (perfMon, perf::Id::MenuBuild);
        menuNames = names;

        fileBox.clear (juce::dontSendNotification);

        for (int i = 0; i < n; ++i)
        {
            const auto title = juce::String (i + 1) + ":  " + names[i];

            // The row's "x" asks us to remove file i. It runs after the menu has closed.
            auto remove = [safe = juce::Component::SafePointer<LayerPage> (this), i]
            {
                juce::MessageManager::callAsync ([safe, i]
                {
                    if (safe != nullptr)
                        safe->library.removeSample (i);
                });
            };

            fileBox.getRootMenu()->addCustomItem (i + 1, std::make_unique<FileMenuItem> (title, std::move (remove)),
                                                  nullptr, title);
        }
    }

    if (n > 0)
        fileBox.setSelectedId (sel + 1, juce::dontSendNotification);

    fileBox.setEnabled (n > 0);
    clearButton.setEnabled (n > 0 || library.getNumPending() > 0);
    undoButton.setEnabled (library.canUndo());
    redoButton.setEnabled (library.canRedo());

    const auto sample = library.getSample (sel);
    waveform.setSample (sample);
    selection.setSample (sample);

    statusText = juce::String (n) + " / " + juce::String (SampleLibrary::kMaxSamples) + " files loaded";

    if (library.getNumPending() > 0)
        statusText << "   |   loading " << library.getNumPending() << "...";

    statusIsWarning = library.getStatusMessage().isNotEmpty();
    if (statusIsWarning)
        statusText << "   |   " << library.getStatusMessage();

    if (onSelectedSampleChanged)  onSelectedSampleChanged (index, sample);
    if (onStatusChanged)          onStatusChanged();
}
