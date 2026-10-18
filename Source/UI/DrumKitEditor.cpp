#include "DrumKitEditor.h"

namespace df
{

namespace
{
    const char* tabName (EditorUiState::Mode mode) noexcept
    {
        switch (mode)
        {
            case EditorUiState::Mode::synth: return "SYNTH";
            case EditorUiState::Mode::mod:   return "MOD";
            case EditorUiState::Mode::seq:   return "SEQ";
            case EditorUiState::Mode::automation: return "AUTO";
            case EditorUiState::Mode::fx:    return "FX";
        }

        return "";
    }

    // Menu ids. Kit entries occupy 1000 + index, note maps 2000 + index.
    constexpr int menuSaveKit     = 1;
    constexpr int menuImportKit   = 2;
    constexpr int menuExportKit   = 3;
    constexpr int menuDeleteKit   = 4;
    constexpr int menuShowFolder  = 5;
    constexpr int menuKitBase     = 1000;
    constexpr int menuNoteMapBase = 2000;
}

//==============================================================================
DrumKitEditor::DrumKitEditor (DrumKitProcessor& owner)
    : juce::AudioProcessorEditor (owner)
{
    setLookAndFeel (&lookAndFeel);

    // ---- Kit name and the main menu -----------------------------------------
    kitLabel.setJustificationType (juce::Justification::centredLeft);
    kitLabel.setFont (theme::font (12.0f));
    kitLabel.setTooltip ("Click to change kit.");
    kitLabel.onClick = [this] { showKitMenu(); };
    addAndMakeVisible (kitLabel);

    menuButton.setTooltip ("Kits, and the MIDI note map.");
    menuButton.onClick = [this] { showMainMenu(); };
    addAndMakeVisible (menuButton);

    // ---- Mode tabs -----------------------------------------------------------
    for (int i = 0; i < (int) tabButtons.size(); ++i)
    {
        auto& button = tabButtons[(size_t) i];
        button.setButtonText (tabName ((EditorUiState::Mode) i));
        button.setClickingTogglesState (false);
        button.onClick = [this, i] { setMode ((EditorUiState::Mode) i); };
        addAndMakeVisible (button);
    }

    // ---- Track rows ----------------------------------------------------------
    for (int t = 0; t < kNumTracks; ++t)
    {
        strips[(size_t) t] = std::make_unique<TrackStrip> (kit(), t, uiState);
        addAndMakeVisible (*strips[(size_t) t]);
    }

    // ---- Sequencer lanes -----------------------------------------------------
    for (int t = 0; t < kNumTracks; ++t)
    {
        lanes[(size_t) t] = std::make_unique<SequencerLaneComponent> (kit(), t);
        addAndMakeVisible (*lanes[(size_t) t]);

        automationLanes[(size_t) t] = std::make_unique<AutomationLaneComponent> (kit(), t);
        addAndMakeVisible (*automationLanes[(size_t) t]);
    }

    // ---- Global knobs --------------------------------------------------------
    // The reverb lives on its own page. The master level has no knob at all: it stays an
    // automatable parameter, but the top bar is worth more than a control nobody needs to
    // reach for on every session.
    const char* reverbIDs[4] { pid::reverbSize, pid::reverbDamp, pid::reverbMix, pid::reverbWidth };
    const char* reverbNames[4] { "Size", "Damp", "Mix", "Width" };
    const char* reverbTips[4]
    {
        "Reverb room size",
        "Reverb high-frequency damping",
        "How much reverb is blended into the master bus",
        "Stereo width of the reverb"
    };

    for (int i = 0; i < 4; ++i)
    {
        auto& knob = reverbKnobs[(size_t) i];
        knob = std::make_unique<ModelKnob> (kit(), -1, -1, uiState, reverbIDs[i]);
        knob->setDisplayName (reverbNames[i]);
        knob->setAccentColour (juce::Colour (0xff7f8ea8));
        knob->setTooltip (reverbTips[i]);
        addAndMakeVisible (*knob);
    }

    currentKitIndex = kit().getSelectedKit();
    lastSeenKitParameter = currentKitIndex;
    refreshKitLabel();

    setMode (EditorUiState::Mode::synth);

    setResizable (true, true);
    setResizeLimits (1120, 720, 2400, 1500);
    setSize (1360, 860);

    startTimerHz (30);
}

DrumKitEditor::~DrumKitEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

//==============================================================================
void DrumKitEditor::setMode (EditorUiState::Mode mode)
{
    uiState.mode.store (mode);

    for (int i = 0; i < (int) tabButtons.size(); ++i)
    {
        auto& button = tabButtons[(size_t) i];
        const bool active = (int) mode == i;

        button.setToggleState (active, juce::dontSendNotification);
        button.setColour (juce::TextButton::buttonColourId,
                          active ? theme::accent : theme::panel);
        button.setColour (juce::TextButton::textColourOffId,
                          active ? theme::background : theme::textDim);
        button.setColour (juce::TextButton::textColourOnId,
                          active ? theme::background : theme::textDim);
    }

    updateContentVisibility();

    // A lane's type decides which of its controls are visible, so a lane shown for the first
    // time has to re-sync before it is laid out.
    for (auto& lane : lanes)
        lane->refreshFromProcessor();

    for (auto& lane : automationLanes)
        lane->refreshFromProcessor();

    resized();
    repaint();
}

void DrumKitEditor::updateContentVisibility()
{
    const auto mode = uiState.mode.load();
    const bool showTracks = mode == EditorUiState::Mode::synth
                              || mode == EditorUiState::Mode::mod;

    for (auto& strip : strips)
        strip->setVisible (showTracks);

    for (auto& knob : reverbKnobs)
        knob->setVisible (mode == EditorUiState::Mode::fx);

    for (auto& lane : lanes)
        lane->setVisible (mode == EditorUiState::Mode::seq);

    for (auto& lane : automationLanes)
        lane->setVisible (mode == EditorUiState::Mode::automation);
}

//==============================================================================
void DrumKitEditor::resized()
{
    auto area = getLocalBounds();

    // ---- Top bar -------------------------------------------------------------
    topBarArea = area.removeFromTop (topBarHeight);
    {
        auto bar = topBarArea.reduced (16, 3);

        titleArea = bar.removeFromLeft (juce::jmin (200, bar.getWidth() / 4));

        menuButtonArea = bar.removeFromRight (46).withSizeKeepingCentre (46, 18);
        menuButton.setBounds (menuButtonArea);
        bar.removeFromRight (12);

        kitLabelArea = bar.reduced (6, 0);
        kitLabel.setBounds (kitLabelArea);
    }

    // ---- Tab bar -------------------------------------------------------------
    tabBarArea = area.removeFromTop (tabBarHeight);
    {
        auto tabs = tabBarArea.reduced (16, 4);

        for (auto& button : tabButtons)
            button.setBounds (tabs.removeFromLeft (94).reduced (2, 0));

        hintArea = tabs.reduced (10, 0);
    }

    // ---- Content -------------------------------------------------------------
    trackArea = area.reduced (10, 8);
    seqArea = trackArea;
    automationArea = trackArea;
    fxArea = trackArea;

    {
        auto row = fxArea.reduced (40, 46);
        const int cellWidth = juce::jmin (180, juce::jmax (80, row.getWidth() / 5));
        auto cells = row.withSizeKeepingCentre (cellWidth * 4, juce::jmin (row.getHeight(), 190));

        for (auto& knob : reverbKnobs)
            knob->setBounds (cells.removeFromLeft (cellWidth));
    }

    const int rowHeight = trackArea.getHeight() / kNumTracks;

    for (int t = 0; t < kNumTracks; ++t)
    {
        strips[(size_t) t]->setBounds (trackArea.getX(),
                                       trackArea.getY() + t * rowHeight,
                                       trackArea.getWidth(),
                                       rowHeight);

        lanes[(size_t) t]->setBounds (seqArea.getX(),
                                      seqArea.getY() + t * rowHeight,
                                      seqArea.getWidth(),
                                      rowHeight);

        automationLanes[(size_t) t]->setBounds (automationArea.getX(),
                                                automationArea.getY() + t * rowHeight,
                                                automationArea.getWidth(),
                                                rowHeight);
    }
}

void DrumKitEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);

    // ---- Wordmark ------------------------------------------------------------
    g.setColour (theme::accent);
    g.setFont (theme::font (15.0f, juce::Font::bold));
    g.drawText ("InfinityDrum", titleArea, juce::Justification::centredLeft, false);

    // ---- Tab bar rule and the mode hint --------------------------------------
    g.setColour (theme::outline);
    g.fillRect (tabBarArea.getX() + 16, tabBarArea.getBottom() - 1,
                tabBarArea.getWidth() - 32, 1);

    if (! hintArea.isEmpty() && uiState.mode.load() == EditorUiState::Mode::mod)
    {
        g.setColour (theme::velocityHard);
        g.setFont (theme::font (11.0f, juce::Font::bold));
        g.drawText ("Velocity modulation  -  drag any knob to set its depth",
                    hintArea, juce::Justification::centredRight, false);
    }

    // ---- FX page ------------------------------------------------------------
    if (uiState.mode.load() == EditorUiState::Mode::fx)
    {
        const auto panel = fxArea.toFloat();

        g.setColour (theme::panel);
        g.fillRoundedRectangle (panel, 6.0f);
        g.setColour (theme::outline);
        g.drawRoundedRectangle (panel, 6.0f, 1.0f);

        auto header = fxArea.reduced (26, 20).removeFromTop (40);

        g.setColour (theme::text);
        g.setFont (theme::font (14.0f, juce::Font::bold));
        g.drawText ("REVERB", header, juce::Justification::centredLeft, false);

        g.setColour (theme::textDim);
        g.setFont (theme::font (11.0f));
        g.drawText ("Shared across all eight tracks. Each track's Send decides how much "
                    "reaches it.", header, juce::Justification::centredRight, false);
    }

}

//==============================================================================
juce::PopupMenu DrumKitEditor::buildKitMenu()
{
    juce::PopupMenu kits;
    const int userStart = numFactoryKits();

    for (int i = 0; i < userStart; ++i)
        kits.addItem (menuKitBase + i, library.getName (i), true, i == currentKitIndex);

    if (library.getNumKits() > userStart)
    {
        kits.addSeparator();

        for (int i = userStart; i < library.getNumKits(); ++i)
            kits.addItem (menuKitBase + i, library.getName (i), true, i == currentKitIndex);
    }

    kits.addSeparator();
    kits.addItem (menuSaveKit, "Save current settings as a kit...");
    kits.addItem (menuImportKit, "Import a kit file...");
    kits.addItem (menuExportKit, "Export the current kit...");

    if (! library.isFactoryKit (currentKitIndex))
        kits.addItem (menuDeleteKit, "Delete \"" + library.getName (currentKitIndex) + "\"");

    kits.addItem (menuShowFolder, "Show kits folder");

    return kits;
}

void DrumKitEditor::showKitMenu()
{
    buildKitMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&kitLabel),
                                  [this] (int result) { handleMainMenuResult (result); });
}

void DrumKitEditor::showMainMenu()
{
    juce::PopupMenu menu;

    menu.addSubMenu ("Kit", buildKitMenu());

    // ---- Note map -----------------------------------------------------------
    juce::PopupMenu maps;

    for (int i = 0; i < kNumNoteMaps; ++i)
        maps.addItem (menuNoteMapBase + i, noteMapName ((NoteMap) i), true,
                      (int) kit().getNoteMap() == i);

    menu.addSubMenu ("MIDI note map", maps);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (menuButton),
                        [this] (int result) { handleMainMenuResult (result); });
}

void DrumKitEditor::handleMainMenuResult (int result)
{
    if (result >= menuNoteMapBase)
    {
        kit().setNoteMap ((NoteMap) (result - menuNoteMapBase));
        repaint();
        return;
    }

    if (result >= menuKitBase)
    {
        loadKit (result - menuKitBase);
        return;
    }

    switch (result)
    {
        case menuSaveKit:    saveCurrentKitAs();       break;
        case menuImportKit:  importKitFromFile();      break;
        case menuExportKit:  exportCurrentKitToFile(); break;
        case menuDeleteKit:  deleteCurrentKit();       break;

        case menuShowFolder:
            library.getUserDirectory().createDirectory();
            library.getUserDirectory().revealToUser();
            break;

        default: break;
    }
}

//==============================================================================
void DrumKitEditor::loadKit (int index)
{
    if (! juce::isPositiveAndBelow (index, library.getNumKits()))
        return;

    // applyKitPreset keeps the host-facing kit parameter in step with the built-in kits;
    // a user kit has no parameter to write, so it is applied directly.
    if (library.isFactoryKit (index))
        kit().applyKitPreset (index);

    applyKit (kit(), library.getKit (index));

    currentKitIndex = index;

    // Record what the kit parameter now reads. Without this the timer would compare it with
    // the old value on every tick, decide the host had changed the kit, and re-apply it -
    // which silently reverted every knob the user then touched.
    lastSeenKitParameter = kit().getSelectedKit();

    refreshKitLabel();
}

void DrumKitEditor::saveCurrentKitAs()
{
    const auto suggested = juce::isPositiveAndBelow (currentKitIndex, library.getNumKits())
                             ? library.getName (currentKitIndex) + " Copy"
                             : juce::String ("My Kit");

    auto* window = new juce::AlertWindow ("Save kit",
                                          "Save everything currently dialled in as a kit:",
                                          juce::MessageBoxIconType::NoIcon);
    window->addTextEditor ("name", suggested, "Name:");
    window->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    window->enterModalState (true,
        juce::ModalCallbackFunction::create ([this, window] (int result)
        {
            if (result != 1)
                return;

            const auto snapshot = captureKit (kit(), window->getTextEditorContents ("name"));

            if (const auto saved = library.saveKit (snapshot); saved.failed())
            {
                juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                                  .withIconType (juce::MessageBoxIconType::WarningIcon)
                                                  .withTitle ("Could not save kit")
                                                  .withMessage (saved.getErrorMessage())
                                                  .withButton ("OK"),
                                              nullptr);
                return;
            }

            currentKitIndex = library.indexOfName (snapshot.name);
            refreshKitLabel();
        }),
        true);
}

void DrumKitEditor::importKitFromFile()
{
    auto chooser = std::make_shared<juce::FileChooser> (
        "Import a kit",
        juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
        "*" + KitLibrary::fileExtension());

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectFiles,
        [this, chooser] (const juce::FileChooser& fc)
        {
            const auto file = fc.getResult();

            if (! file.existsAsFile())
                return;

            auto xml = juce::XmlDocument::parse (file);

            if (xml == nullptr)
                return;

            KitData imported;

            if (! kitFromXml (*xml, imported))
                return;

            if (imported.name.isEmpty())
                imported.name = file.getFileNameWithoutExtension();

            applyKit (kit(), imported);

            // Importing also adds it to the library, so it is there next time.
            library.saveKit (imported);
            currentKitIndex = library.indexOfName (imported.name);
            refreshKitLabel();
        });
}

void DrumKitEditor::exportCurrentKitToFile()
{
    const auto suggestedName = juce::isPositiveAndBelow (currentKitIndex, library.getNumKits())
                                 ? library.getName (currentKitIndex)
                                 : juce::String ("InfinityDrum Kit");

    auto chooser = std::make_shared<juce::FileChooser> (
        "Export the current kit",
        juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
            .getChildFile (suggestedName + KitLibrary::fileExtension()),
        "*" + KitLibrary::fileExtension());

    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                            | juce::FileBrowserComponent::canSelectFiles
                            | juce::FileBrowserComponent::warnAboutOverwriting,
        [this, chooser] (const juce::FileChooser& fc)
        {
            auto file = fc.getResult();

            if (file == juce::File())
                return;

            if (file.getFileExtension().isEmpty())
                file = file.withFileExtension (KitLibrary::fileExtension());

            const auto snapshot = captureKit (kit(), file.getFileNameWithoutExtension());

            if (auto xml = kitToXml (snapshot))
                xml->writeTo (file);
        });
}

void DrumKitEditor::deleteCurrentKit()
{
    if (library.isFactoryKit (currentKitIndex))
        return;

    library.deleteKit (currentKitIndex);

    // The sound is still loaded; it just no longer corresponds to anything on disk.
    currentKitIndex = -1;
    refreshKitLabel();
}

void DrumKitEditor::refreshKitLabel()
{
    const bool known = juce::isPositiveAndBelow (currentKitIndex, library.getNumKits());

    kitLabel.setText (known ? "Kit:  " + library.getName (currentKitIndex)
                            : juce::String ("Kit:  (unsaved)"),
                      juce::dontSendNotification);
}

//==============================================================================
void DrumKitEditor::timerCallback()
{
    refreshFromProcessor();
}

void DrumKitEditor::refreshFromProcessor()
{
    // Polled rather than driven by an attachment: restoring a session changes the kit
    // parameter, and that must not re-apply the preset over the restored values. Only a
    // change we did not make ourselves - automation, or a session load - gets acted on.
    if (const int selected = kit().getSelectedKit(); selected != lastSeenKitParameter)
        loadKit (selected);

    for (auto& strip : strips)
        strip->updateFromProcessor();

    // Both lane pages repaint from here. The playheads are the reason: they read a position
    // the audio thread writes every block, so a lane that is not repainted on the timer has
    // a playhead that only moves when something else happens to redraw it - a mouse event,
    // or a page change. That is exactly how the automation playhead shipped broken.
    for (auto& lane : lanes)
        lane->refreshFromProcessor();

    for (auto& lane : automationLanes)
        lane->refreshFromProcessor();
}

juce::String DrumKitEditor::getTrackNoteLabel (int trackIndex) const
{
    if (! juce::isPositiveAndBelow (trackIndex, kNumTracks))
        return {};

    return strips[(size_t) trackIndex]->getNoteLabel();
}

} // namespace df
