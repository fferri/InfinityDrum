#pragma once

#include <array>
#include <memory>

#include <juce_audio_processors/juce_audio_processors.h>

#include "../Core/DrumKitProcessor.h"
#include "../Core/KitLibrary.h"
#include "ModelKnob.h"
#include "SequencerLane.h"
#include "Theme.h"
#include "TrackStrip.h"
#include "UiState.h"

namespace df
{

/** The plugin window.

    Three bands: a top bar holding the wordmark, the current kit name, the global knobs and
    the main menu; a tab bar switching between SYNTH, MOD and SEQ; and the content area,
    which is eight track rows or, on the sequencer page, its placeholder.

    Everything app-level lives in the main menu rather than on the tracks: kits (load, save,
    import, export, delete) and the MIDI note map. Right-clicking a track pad used to offer
    the note map; that moved here because it is a property of the whole instrument.
*/
class DrumKitEditor final : public juce::AudioProcessorEditor,
                            private juce::Timer
{
public:
    explicit DrumKitEditor (DrumKitProcessor&);
    ~DrumKitEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** One editor tick: what the timer runs, exposed so a test can drive it directly. */
    void refreshFromProcessor();

    /** What a track's header note caption currently reads. */
    juce::String getTrackNoteLabel (int trackIndex) const;

    /** Switches page. Public so a test can drive it without synthesising clicks. */
    void setMode (EditorUiState::Mode mode);
    EditorUiState::Mode getMode() const noexcept { return uiState.mode.load(); }

private:
    void timerCallback() override;

    void showMainMenu();
    void showKitMenu();

    /** The kit list and its file commands. Shared by the main menu and the kit name. */
    juce::PopupMenu buildKitMenu();
    void handleMainMenuResult (int result);

    void loadKit (int index);
    void saveCurrentKitAs();
    void importKitFromFile();
    void exportCurrentKitToFile();
    void deleteCurrentKit();
    void refreshKitLabel();
    void updateContentVisibility();

    /** The editor's own processor, already available as `processor` on the base class. */
    DrumKitProcessor& kit() const noexcept { return static_cast<DrumKitProcessor&> (processor); }

    static constexpr int topBarHeight = 24;
    static constexpr int tabBarHeight = 34;

    InfinityDrumLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltipWindow { this, 650 };

    KitLibrary library;

    KitNameButton kitLabel;
    MenuButton menuButton;
    std::array<juce::TextButton, 5> tabButtons;

    EditorUiState uiState;

    std::array<std::unique_ptr<TrackStrip>, (size_t) kNumTracks> strips;
    std::array<std::unique_ptr<SequencerLaneComponent>, (size_t) kNumTracks> lanes;
    std::array<std::unique_ptr<AutomationLaneComponent>, (size_t) kNumTracks> automationLanes;
    std::array<std::unique_ptr<ModelKnob>, 4> reverbKnobs;

    juce::Rectangle<int> topBarArea, titleArea, kitLabelArea, menuButtonArea,
                        tabBarArea, hintArea, trackArea, seqArea, automationArea, fxArea;

    /** The kit the editor last loaded, or -1 once the current sound matches no kit. */
    int currentKitIndex = -1;

    /** The last value of the host-facing kit parameter that we ourselves put there. */
    int lastSeenKitParameter = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumKitEditor)
};

} // namespace df
