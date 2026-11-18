#pragma once

#include <array>

#include <juce_audio_processors/juce_audio_processors.h>

#include "DrumTrack.h"
#include "MarkovChain.h"
#include "ModulationState.h"
#include "Sequencer.h"
#include "Parameters.h"

namespace df
{

/** The instrument: eight independently assignable drum tracks, a GM note map,
    a shared reverb send and a master bus.

    Audio-thread entry points are `processBlock` and the lock-free `triggerTrack`,
    which lets the UI audition a pad without ever touching engine state directly.
*/
class DrumKitProcessor final : public juce::AudioProcessor
{
public:
    DrumKitProcessor();
    ~DrumKitProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "InfinityDrum"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 12.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    //==============================================================================
    // Used by the editor and by the offline rendering tools.

    /** Queues a hit for the audio thread. Safe to call from the message thread. */
    void triggerTrack (int trackIndex, float velocity);

    /** Reads the track's peak since the last call, and clears it. Read-and-clear, so it
        must not be called twice for one observation - the second call returns zero. */
    float readTrackPeak (int trackIndex) noexcept;
    uint32_t getTrackTriggerCount (int trackIndex) const noexcept;

    /** Writes the given engine's own default values into a track's four macro knobs. */
    void applyModelDefaults (int trackIndex, int modelIndex);

    /** As above, using whichever engine the track currently has selected. */
    void applyModelDefaults (int trackIndex);

    /** Loads a complete factory kit: engines, distortion modes and every track setting. */
    void applyKitPreset (int kitIndex);

    /** Index of the kit currently selected (reads the parameter). */
    int getSelectedKit() const noexcept;

    //==============================================================================
    // Velocity modulation. Depths are deliberately not parameters, so they have their own
    // accessors rather than going through apvts.getParameter().

    /** Which MIDI note map the pads respond to.

        It is a real parameter, so it is saved with the session and visible to the host -
        a project that used a BeatStep reopens on the BeatStep map. Its *default* comes
        from the user's global preference instead, so a one-time choice still applies to
        every new instance. */
    NoteMap getNoteMap() const noexcept;
    void setNoteMap (NoteMap map);

    ModulationState& getModulationState() noexcept { return modulation; }

    /** The sequencer lanes. Message thread; the audio thread reads the same object. */
    SequencerState& getSequencerState() noexcept { return sequencer; }
    const SequencerState& getSequencerState() const noexcept { return sequencer; }

    /** The pattern bank and the Markov chain that selects from it. */
    MarkovChain& getMarkovChain() noexcept { return markovChain; }
    const MarkovChain& getMarkovChain() const noexcept { return markovChain; }

    Pattern& getPattern (int index) noexcept
    {
        return patterns[(size_t) juce::jlimit (0, kNumPatterns - 1, index)];
    }

    const Pattern& getPattern (int index) const noexcept
    {
        return patterns[(size_t) juce::jlimit (0, kNumPatterns - 1, index)];
    }

    /** Copies the live SEQ/AUTO state into a pattern slot, and marks the slot used. */
    void capturePattern (int index);

    /** Copies a used pattern back into the live state, for editing on the SEQ page. */
    void loadPattern (int index);

    void clearPattern (int index);

    /** Which state the chain is currently in, or -1 while the chain is off. */
    int getChainActiveState() const noexcept { return chainActiveState.load(); }
    int getChainActivePattern() const noexcept { return chainActivePattern.load(); }

    /** Bumped whenever a session or a pattern capture changes the data the editor shows. */
    uint32_t getUiGeneration() const noexcept { return uiGeneration.load(); }

    /** Where a lane's cycle sits, 0..1, for the editor's playhead. */
    float getTriggerPhase (int trackIndex) const noexcept
    {
        return triggerPhase[(size_t) juce::jlimit (0, kNumTracks - 1, trackIndex)].load();
    }

    float getAutomationPhase (int trackIndex, int slot) const noexcept
    {
        return automationPhase[(size_t) juce::jlimit (0, kNumTracks - 1, trackIndex)]
                              [(size_t) juce::jlimit (0, kMaxAutomationLanes - 1, slot)].load();
    }
    const ModulationState& getModulationState() const noexcept { return modulation; }

    /** Sets how far a velocity sweep moves one parameter. `depth` is -1..1. Message thread. */
    void setModulationDepth (int trackIndex, int modTarget, float depth);

    float getModulationDepth (int trackIndex, int modTarget) const noexcept;

    /** Velocity of the most recent hit on a track, used to draw the modulation band. */
    float getLastVelocity (int trackIndex) const noexcept;

    /** Index of the model currently selected on a track (reads the parameter). */
    int getSelectedModel (int trackIndex) const noexcept;

    /** Model currently instantiated on a track. Only meaningful while audio runs. */
    int getActiveModel (int trackIndex) const noexcept;

private:
    struct PendingTrigger
    {
        int track = 0;
        float velocity = 1.0f;
    };

    void handleMidiMessage (const juce::MidiMessage& message);
    void triggerTrackInternal (int trackIndex, float velocity);
    void applyModelSelection();
    void renderChunk (int numSamples);

    /** Resolves one track's parameters for a hit at the given velocity. */
    ModelParams buildTrackParams (int trackIndex, float velocity) const;

    static void setParameterValue (juce::AudioProcessorValueTreeState& state,
                                   const juce::String& parameterID,
                                   float value);

    std::array<DrumTrack, kNumTracks> tracks;
    std::array<TrackParamRefs, kNumTracks> trackRefs {};
    std::array<TrackModRefs, kNumTracks> modRefs {};
    GlobalParamRefs globalRefs;

    juce::Reverb reverb;
    juce::AudioBuffer<float> mixBuffer;
    juce::AudioBuffer<float> sendBuffer;
    int preparedBlockSize = 512;

    std::array<bool, kNumTracks> blockMuted {};
    std::array<bool, kNumTracks> blockSoloed {};
    bool anySolo = false;

    // Modulation is latched per track at note-on, so a ringing hit keeps the character it
    // was struck with until the next hit re-latches it. Atomic because the editor also
    // reads it, to place the dot on the modulation band.
    std::array<std::atomic<float>, kNumTracks> lastVelocity;

    ModulationState modulation;

    /** The eight lanes. Written on the message thread by the sequencer page and read on the
        audio thread; every field is a plain scalar, so the worst a race can do is let one
        block hear a half-applied edit, never crash or wedge a voice. */
    SequencerState sequencer;

    /** The pattern bank: snapshots of the live state, plus the chain that selects among
        them. Like the lanes, the audio thread reads them on the message thread's word; a
        race can only let one block hear a half-applied pattern. */
    std::array<Pattern, (size_t) kNumPatterns> patterns {};

    MarkovChain markovChain;

    /** Audio-thread only: the chain's position, and the mirrors the editor reads. */
    ChainPlayer chainPlayer;
    std::atomic<int> chainActiveState { -1 };
    std::atomic<int> chainActivePattern { -1 };
    std::atomic<uint32_t> uiGeneration { 0 };
    std::atomic<bool> chainNeedsReset { false };
    bool wasChainEnabled = false;

    /** Audio-thread only: turns transport position into lane actions. */
    SequencerTransport transport;

    /** Audio-thread only: this block's triggers, waiting to be fired at their sample. */
    SequencerOutput pendingTriggers;

    /** Mirrors of the transport's playhead position, for the editor. */
    std::array<std::atomic<float>, (size_t) kNumTracks> triggerPhase {};
    std::array<std::array<std::atomic<float>, (size_t) kMaxAutomationLanes>,
               (size_t) kNumTracks> automationPhase {};

    /** Audio-thread only: the offset each modulation lane contributes this block. */
    std::array<std::array<float, (size_t) kNumModTargets>, (size_t) kNumTracks> sequenceOffsets {};

    Rng triggerRng { 0x51ed2701u };

    // Single-producer / single-consumer queue for UI-triggered hits.
    static constexpr int triggerQueueSize = 128;
    juce::AbstractFifo triggerFifo { triggerQueueSize };
    std::array<PendingTrigger, (size_t) triggerQueueSize> triggerQueue {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumKitProcessor)
};

} // namespace df
