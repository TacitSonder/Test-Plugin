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
    setColour (juce::Slider::backgroundColourId, theme::panel);
    setColour (juce::Slider::trackColourId, theme::accent.withAlpha (0.55f));
    setColour (juce::Slider::thumbColourId, theme::accent);
    setColour (juce::Slider::textBoxTextColourId, theme::text);
    setColour (juce::Slider::textBoxOutlineColourId, theme::outline);
    setColour (juce::Slider::rotarySliderFillColourId, theme::accent);
    setColour (juce::TextButton::buttonColourId, theme::panel);
    setColour (juce::TextButton::buttonOnColourId, theme::accent.withAlpha (0.45f));
    setColour (juce::TextButton::textColourOffId, theme::text);
    setColour (juce::TextButton::textColourOnId, theme::text);
}

void SamplerLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float pos,
                                           float startAngle, float endAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (6.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float thickness = juce::jmax (4.0f, radius * 0.2f);
    const float arcRadius = radius - thickness * 0.5f;
    const float angle = startAngle + pos * (endAngle - startAngle);

    juce::Path track, value;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
    value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, angle, true);

    g.setColour (theme::background);
    g.strokePath (track, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
    g.setColour (slider.findColour (juce::Slider::rotarySliderFillColourId));
    g.strokePath (value, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::butt));

    const float td = thickness * 1.5f;                                          // thumb
    g.setColour (juce::Colours::white);
    g.fillEllipse (juce::Rectangle<float> (td, td).withCentre ({ centre.x + arcRadius * std::sin (angle),
                                                                  centre.y - arcRadius * std::cos (angle) }));

    g.setColour (theme::subtle);                                                // value in the middle
    g.setFont (juce::FontOptions (juce::jlimit (11.0f, 22.0f, radius * 0.36f)));
    g.drawFittedText (slider.getTextFromValue (slider.getValue()),
                      juce::Rectangle<float> (radius * 1.3f, radius * 0.6f).withCentre (centre).toNearestInt(),
                      juce::Justification::centred, 1, 0.6f);
}

void SamplerLookAndFeel::drawTabButton (juce::TabBarButton& button, juce::Graphics& g, bool isMouseOver, bool)
{
    const auto area = button.getActiveArea().toFloat();
    const auto colour = button.getTabBackgroundColour();
    const bool front = button.isFrontTab();
    const float slant = area.getHeight() * 0.45f;

    juce::Path shape;                                   // a tab with slanted sides, like the reference design
    shape.startNewSubPath (area.getX(), area.getBottom());
    shape.lineTo (area.getX() + slant, area.getY());
    shape.lineTo (area.getRight() - slant, area.getY());
    shape.lineTo (area.getRight(), area.getBottom());
    shape.closeSubPath();

    g.setColour (colour.withAlpha (front ? 1.0f : isMouseOver ? 0.55f : 0.35f));
    g.fillPath (shape);

    g.setColour (front ? theme::textOn (colour) : theme::text);
    g.setFont (juce::FontOptions (14.0f));
    g.drawText (button.getButtonText(), area.toNearestInt(), juce::Justification::centred, false);
}

int SamplerLookAndFeel::getTabButtonBestWidth (juce::TabBarButton&, int) { return 120; }

void SamplerLookAndFeel::drawTabAreaBehindFrontButton (juce::TabbedButtonBar&, juce::Graphics&, int, int) {}

// -----------------------------------------------------------------------------
SamplerEditor::SamplerEditor (SamplerProcessor& p)
    : AudioProcessorEditor (&p),
      processor (p),
      perfMon (p.getPerf()),
      timeline (p.getPerf()),
      overlay (p.getPerf(), [&p]
      {
          size_t bytes = 0;
          int files = 0;
          for (int i = 0; i < layercfg::kCount; ++i)
          {
              bytes += p.getLibrary (i).getTotalBytes();
              files += p.getLibrary (i).getNumLoaded();
          }
          return "samples: " + juce::String (files) + " loaded, " + juce::String ((double) bytes / (1024.0 * 1024.0), 1) + " MB";
      })
{
    setLookAndFeel (&lookAndFeel);
    setOpaque (true);
    setWantsKeyboardFocus (true);

    for (int i = 0; i < layercfg::kCount; ++i)
    {
        tabs.addTab (layercfg::kNames[i], theme::layerColour (i), -1);

        pages[(size_t) i] = std::make_unique<LayerPage> (processor, i);
        pages[(size_t) i]->onSelectedSampleChanged = [this] (int layer, std::shared_ptr<const SampleData> s) { timeline.setSample (layer, std::move (s)); };
        pages[(size_t) i]->onStatusChanged = [this] { repaint (statusArea); };
        addChildComponent (*pages[(size_t) i]);

        timeline.setLayerStyle (i, layercfg::kNames[i], theme::layerColour (i));
        timeline.setSample (i, processor.getLibrary (i).getSample (processor.getLibrary (i).getSelected()));
    }

    tabs.onChange = [this] (int layer) { setActiveLayer (layer); };
    addAndMakeVisible (tabs);

    playButton.setComponentID ("playButton");
    playButton.setTooltip ("Plays every layer (MIDI note 60, middle C)");
    playButton.setTriggeredOnMouseDown (true);          // fire on press, not on release
    playButton.onClick = [this] { processor.triggerFromGui(); };
    addAndMakeVisible (playButton);

    timeline.onDelayChanged = [this] (int layer, float ms, bool committed)
    {
        auto& lib = processor.getLibrary (layer);
        lib.setDelay (lib.getSelected(), ms, committed);
    };
    addAndMakeVisible (timeline);

    scaleSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    scaleSlider.setRange (100.0, 5000.0, 1.0);
    scaleSlider.setSkewFactorFromMidPoint (700.0);       // short and long files both get fine control
    scaleSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 78, 22);
    scaleSlider.setTextValueSuffix (" ms");
    scaleSlider.setDoubleClickReturnValue (true, 1000.0);
    scaleSlider.onValueChange = [this]
    {
        const int ms = (int) scaleSlider.getValue();
        timeline.setScaleMs (ms);
        processor.setScaleMs (ms);
    };
    scaleSlider.setValue ((double) processor.getScaleMs(), juce::dontSendNotification);
    timeline.setScaleMs (processor.getScaleMs());
    addAndMakeVisible (scaleSlider);

#if SAMPLER_PERF_ENABLED
    perfButton.setComponentID ("perfButton");
    perfButton.setClickingTogglesState (true);
    perfButton.onClick = [this]
    {
        overlay.setActive (perfButton.getToggleState());
        layoutOverlay();
    };
    addAndMakeVisible (perfButton);
    addChildComponent (overlay);   // added last so it paints on top of everything
#endif

    // Read the saved size first: setResizeLimits() clamps the (still empty) editor to the
    // minimum size, and resized() would overwrite the saved size with it.
    const int savedWidth = processor.getEditorWidth(), savedHeight = processor.getEditorHeight();

    setResizable (true, true);
    setResizeLimits (580, 560, 2400, 1800);
    setSize (savedWidth, savedHeight);

    activeLayer = juce::jlimit (0, layercfg::kCount - 1, processor.getActiveTab());
    tabs.setCurrentTabIndex (activeLayer, false);
    setActiveLayer (activeLayer);
}

SamplerEditor::~SamplerEditor()
{
    setLookAndFeel (nullptr);
}

void SamplerEditor::setActiveLayer (int layer)
{
    activeLayer = juce::jlimit (0, layercfg::kCount - 1, layer);
    processor.setActiveTab (activeLayer);

    for (int i = 0; i < layercfg::kCount; ++i)
        pages[(size_t) i]->setVisible (i == activeLayer);

    timeline.setActiveLayer (activeLayer);
    repaint (statusArea);
}

// ---- layout -----------------------------------------------------------------
void SamplerEditor::resized()
{
    perf::ScopedTimer timer (perfMon, perf::Id::EditorResize);

    processor.setEditorSize (getWidth(), getHeight());

    const int w = getWidth(), h = getHeight();
    const int pad     = juce::jlimit (8, 18, juce::jmin (w, h) / 30);
    const int gap     = juce::jmax (4, pad / 2);
    const int tabH    = juce::jlimit (28, 38, h / 20);
    const int statusH = juce::jlimit (18, 26, h / 28);
    const int buttonW = juce::jlimit (56, 90, w / 9);

    auto r = getLocalBounds().reduced (pad);

    auto top = r.removeFromTop (tabH);
#if SAMPLER_PERF_ENABLED
    perfButton.setBounds (top.removeFromRight (buttonW));
    top.removeFromRight (gap);
#endif
    playButton.setBounds (top.removeFromRight (buttonW));
    top.removeFromRight (gap);
    tabs.setBounds (top);

    r.removeFromTop (gap);
    statusArea = r.removeFromBottom (statusH);
    r.removeFromBottom (gap / 2);
    contentArea = r;

    // bottom: SCALE row, above it the shared timeline; the rest is the active layer's page
    auto scaleRow = r.removeFromBottom (juce::jlimit (22, 30, h / 26));
    scaleLabelArea = scaleRow.removeFromLeft (juce::jlimit (46, 64, w / 12));
    scaleSlider.setBounds (scaleRow.removeFromLeft (juce::jmin (scaleRow.getWidth(), 420)));
    r.removeFromBottom (gap / 2);

    const int timelineH = juce::jmax (TimelineView::getMinimumHeight(), juce::jmin ((int) ((float) r.getHeight() * 0.46f), r.getHeight() - 210));
    timeline.setBounds (r.removeFromBottom (timelineH));
    r.removeFromBottom (gap);

    for (auto& page : pages)
        page->setBounds (r);

    layoutOverlay();
}

void SamplerEditor::layoutOverlay()
{
    if (! overlay.isVisible())
        return;

    const auto wb = contentArea;
    const int ow = juce::jmin (wb.getWidth() - 16, PerfOverlay::kPreferredWidth);
    const int oh = juce::jmin (wb.getHeight() - 16, overlay.getPreferredHeight());
    overlay.setBounds (wb.getRight() - ow - 8, wb.getY() + 8, juce::jmax (0, ow), juce::jmax (0, oh));
}

void SamplerEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);

    g.setColour (theme::subtle);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText ("SCALE", scaleLabelArea, juce::Justification::centredLeft, false);

    g.setFont (juce::FontOptions (juce::jlimit (12.0f, 15.0f, (float) statusArea.getHeight() * 0.62f)));

    auto area = statusArea;

    if (area.getWidth() > 640)
    {
        g.setColour (theme::subtle);
        g.drawText ("Any MIDI note plays every layer", area.removeFromRight (260),
                    juce::Justification::centredRight, true);
    }

    g.setColour (activePage().isStatusWarning() ? theme::warning : theme::subtle);
    g.drawText (juce::String (layercfg::kNames[activeLayer]) + ":  " + activePage().getStatusText(), area,
                juce::Justification::centredLeft, true);
}

bool SamplerEditor::keyPressed (const juce::KeyPress& key)
{
    const auto cmd = juce::ModifierKeys::commandModifier, shift = juce::ModifierKeys::shiftModifier;
    auto& library = activePage().getLibrary();

    if (key == juce::KeyPress ('z', cmd, 0))          { library.undo(); return true; }
    if (key == juce::KeyPress ('y', cmd, 0)
        || key == juce::KeyPress ('z', cmd | shift, 0)) { library.redo(); return true; }

    return false;
}

// ---- drag and drop (goes to the layer whose tab is showing) ----------------------
bool SamplerEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    if (files == lastDragFiles)
        return lastDragResult;

    lastDragFiles = files;
    lastDragResult = false;

    for (const auto& f : files)
        if (activePage().getLibrary().isSupportedFile (juce::File (f)))
        {
            lastDragResult = true;
            break;
        }

    return lastDragResult;
}

void SamplerEditor::fileDragEnter (const juce::StringArray&, int, int)
{
    activePage().setDropHighlight (true);
}

void SamplerEditor::fileDragExit (const juce::StringArray&)
{
    activePage().setDropHighlight (false);
    lastDragFiles.clear();
}

void SamplerEditor::filesDropped (const juce::StringArray& files, int, int)
{
    activePage().setDropHighlight (false);
    lastDragFiles.clear();
    activePage().getLibrary().addFiles (files);
}
