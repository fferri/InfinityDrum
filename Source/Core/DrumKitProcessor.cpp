#include "DrumKitProcessor.h"

#include "KitLibrary.h"
#include "Settings.h"

#include "UI/DrumKitEditor.h"

namespace df
{

DrumKitProcessor::DrumKitProcessor()
    : juce::AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout()),
      modulation (apvts)
{
    patterns = std::make_unique<std::array<Pattern, (size_t) kNumPatterns>>();

    cacheParameterPointers (apvts, trackRefs, globalRefs);
    cacheModulationTargets (apvts, modRefs);

    for (int t = 0; t < kNumTracks; ++t)
    {
        for (int i = 0; i < kNumModTargets; ++i)
            modRefs[(size_t) t].targets[i].depth = modulation.depthPointer (t, i);

        // Until a track has been played there is no hit to react to, so sit on the nominal
        // velocity - which leaves every knob reading exactly as dialled.
        lastVelocity[(size_t) t].store (kNominalVelocity, std::memory_order_relaxed);
    }

    reverb.setSampleRate (48000.0);
}

NoteMap DrumKitProcessor::getNoteMap() const noexcept
{
    if (globalRefs.noteMap == nullptr)
        return NoteMap::generalMidi;

    return (NoteMap) juce::jlimit (0, kNumNoteMaps - 1,
                                   (int) std::lround (globalRefs.noteMap->load()));
}

void DrumKitProcessor::setNoteMap (NoteMap map)
{
    setParameterValue (apvts, pid::noteMap, (float) map);

    // Also remembered globally, so the next new instance starts here rather than making
    // the choice again in every project.
    saveNoteMapSetting (map);
}

void DrumKitProcessor::capturePattern (int index)
{
    if (! juce::isPositiveAndBelow (index, kNumPatterns))
        return;

    (*patterns)[(size_t) index].state = sequencer;
    (*patterns)[(size_t) index].used = true;
    uiGeneration.fetch_add (1, std::memory_order_relaxed);
}

void DrumKitProcessor::loadPattern (int index)
{
    if (! juce::isPositiveAndBelow (index, kNumPatterns) || ! (*patterns)[(size_t) index].used)
        return;

    sequencer = (*patterns)[(size_t) index].state;
    uiGeneration.fetch_add (1, std::memory_order_relaxed);
}

void DrumKitProcessor::clearPattern (int index)
{
    if (! juce::isPositiveAndBelow (index, kNumPatterns))
        return;

    (*patterns)[(size_t) index] = Pattern {};
    uiGeneration.fetch_add (1, std::memory_order_relaxed);
}

void DrumKitProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // A host calls setRateAndBufferSizeDetails before this, but the offline tools and tests
    // call prepareToPlay directly, and without it getSampleRate() stays 0. The sequencer
    // converts beats to samples with it, so a zero rate turned one block into two thousand
    // beats and the lane fired until it hit the event cap.
    if (getSampleRate() <= 0.0)
        setRateAndBufferSizeDetails (sampleRate, samplesPerBlock);

    preparedBlockSize = std::max (64, samplesPerBlock);

    mixBuffer.setSize (2, preparedBlockSize, false, true, true);
    sendBuffer.setSize (2, preparedBlockSize, false, true, true);

    for (auto& track : tracks)
        track.prepare (sampleRate, preparedBlockSize);

    // The tracks were reset to model 0; restore whatever the parameters say.
    applyModelSelection();

    // A host calls prepareToPlay repeatedly. The chain is stateful, so it starts again on
    // the first block after this rather than carrying a position across a reset.
    wasChainEnabled = false;
    chainNeedsReset.store (false, std::memory_order_relaxed);

    reverb.setSampleRate (sampleRate);
}

void DrumKitProcessor::releaseResources()
{
    for (auto& track : tracks)
        track.reset();
}

void DrumKitProcessor::applyModelSelection()
{
    for (int t = 0; t < kNumTracks; ++t)
        tracks[(size_t) t].setModel ((int) std::lround (trackRefs[(size_t) t].model->load()));
}

void DrumKitProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    buffer.clear();

    const int numSamples = buffer.getNumSamples();

    if (numSamples <= 0 || mixBuffer.getNumSamples() < 1)
        return;

    // ---- Snapshot the per-block flags ----------------------------------------
    anySolo = false;

    for (int t = 0; t < kNumTracks; ++t)
    {
        readTrackState (trackRefs[(size_t) t], blockMuted[(size_t) t], blockSoloed[(size_t) t]);
        anySolo = anySolo || blockSoloed[(size_t) t];
    }

    applyModelSelection();

    // ---- Hits queued by the editor -------------------------------------------
    {
        const int ready = triggerFifo.getNumReady();

        if (ready > 0)
        {
            int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
            triggerFifo.prepareToRead (ready, start1, size1, start2, size2);

            for (int i = 0; i < size1; ++i)
                triggerTrackInternal (triggerQueue[(size_t) (start1 + i)].track,
                                      triggerQueue[(size_t) (start1 + i)].velocity);

            for (int i = 0; i < size2; ++i)
                triggerTrackInternal (triggerQueue[(size_t) (start2 + i)].track,
                                      triggerQueue[(size_t) (start2 + i)].velocity);

            triggerFifo.finishedRead (size1 + size2);
        }
    }

    // ---- Sequencer ------------------------------------------------------------
    {
        double ppq = 0.0;
        double bpm = 120.0;
        bool playing = false;

        if (auto* playHead = getPlayHead())
        {
            if (const auto position = playHead->getPosition())
            {
                if (const auto ppqOpt = position->getPpqPosition())
                    ppq = *ppqOpt;

                if (const auto bpmOpt = position->getBpm())
                    bpm = *bpmOpt;

                playing = position->getIsPlaying();
            }
        }

        const double beatsPerSample = bpm / (60.0 * juce::jmax (1.0, getSampleRate()));

        // The chain selects which pattern the transport renders and where in that pattern
        // to start. The transport stays stateless; only the chain remembers anything.
        const SequencerState* source = &sequencer;
        double transportPpq = ppq;

        if (markovChain.enabled)
        {
            const bool resetRequested = chainNeedsReset.exchange (false, std::memory_order_relaxed);

            if (! wasChainEnabled || resetRequested)
                chainPlayer.reset (markovChain, ppq);

            chainPlayer.advance (markovChain, *patterns, sequencer, ppq, beatsPerSample,
                                 numSamples, playing);

            if (const auto* active = chainPlayer.activePattern())
                source = active;

            transportPpq = chainPlayer.virtualPpq();

            const int activeState = juce::jlimit (0, kNumChainStates - 1, chainPlayer.activeState());
            chainActiveState.store (activeState, std::memory_order_relaxed);
            chainActivePattern.store (markovChain.states[(size_t) activeState].pattern,
                                      std::memory_order_relaxed);
        }
        else
        {
            chainActiveState.store (-1, std::memory_order_relaxed);
            chainActivePattern.store (-1, std::memory_order_relaxed);
        }

        wasChainEnabled = markovChain.enabled;

        SequencerOutput output;
        transport.advance (*source, transportPpq, beatsPerSample, numSamples, playing, output);

        for (auto& trackOffsets : sequenceOffsets)
            trackOffsets.fill (0.0f);

        for (int t = 0; t < kNumTracks; ++t)
        {
            sequenceOffsets[(size_t) t] = output.offsets[(size_t) t];

            triggerPhase[(size_t) t].store (output.triggerPhase[(size_t) t],
                                            std::memory_order_relaxed);

            for (int slot = 0; slot < kMaxAutomationLanes; ++slot)
                automationPhase[(size_t) t][(size_t) slot].store (
                    output.automationPhase[(size_t) t][(size_t) slot],
                    std::memory_order_relaxed);
        }

        // The triggers are not fired here. They carry a sample offset, and are fired from
        // inside the render loop below so the hit lands where it belongs in the block.
        pendingTriggers = output;
    }

    // ---- Render, honouring sample-accurate MIDI positions ---------------------
    const int hostChannels = buffer.getNumChannels();

    const auto renderRange = [&] (int start, int length)
    {
        int done = 0;

        while (done < length)
        {
            const int n = std::min (preparedBlockSize, length - done);
            renderChunk (n);

            const int destination = start + done;

            if (hostChannels >= 2)
            {
                buffer.addFrom (0, destination, mixBuffer, 0, 0, n);
                buffer.addFrom (1, destination, mixBuffer, 1, 0, n);
            }
            else if (hostChannels == 1)
            {
                buffer.addFrom (0, destination, mixBuffer, 0, 0, n, 0.5f);
                buffer.addFrom (0, destination, mixBuffer, 1, 0, n, 0.5f);
            }

            done += n;
        }
    };

    int position = 0;
    int nextTrigger = 0;

    // Fires every sequencer trigger at or before `limit`, rendering up to each one first so
    // the decay that follows starts on the right sample.
    const auto fireTriggersUpTo = [&] (int limit)
    {
        while (nextTrigger < pendingTriggers.numTriggers
                 && pendingTriggers.triggers[(size_t) nextTrigger].sampleOffset <= limit)
        {
            const auto& event = pendingTriggers.triggers[(size_t) nextTrigger];

            if (event.sampleOffset > position)
            {
                renderRange (position, event.sampleOffset - position);
                position = event.sampleOffset;
            }

            triggerTrackInternal (event.track, event.velocity);
            ++nextTrigger;
        }
    };

    for (const auto metadata : midiMessages)
    {
        const int eventSample = juce::jlimit (0, numSamples, metadata.samplePosition);

        fireTriggersUpTo (eventSample);

        if (eventSample > position)
        {
            renderRange (position, eventSample - position);
            position = eventSample;
        }

        handleMidiMessage (metadata.getMessage());
    }

    fireTriggersUpTo (numSamples);

    if (position < numSamples)
        renderRange (position, numSamples - position);
}

void DrumKitProcessor::renderChunk (int numSamples)
{
    mixBuffer.clear (0, numSamples);
    sendBuffer.clear (0, numSamples);

    // Each track keeps the velocity of its own last hit, so a ringing sound holds the
    // character it was struck with while other tracks move on.
    for (int t = 0; t < kNumTracks; ++t)
        tracks[(size_t) t].render (mixBuffer, sendBuffer, 0, numSamples,
                                   buildTrackParams (t, lastVelocity[(size_t) t].load (std::memory_order_relaxed)));

    // ---- Shared reverb on the send bus ---------------------------------------
    juce::Reverb::Parameters reverbParameters;
    reverbParameters.roomSize   = globalRefs.reverbSize->load();
    reverbParameters.damping    = globalRefs.reverbDamp->load();
    reverbParameters.width      = globalRefs.reverbWidth->load();
    reverbParameters.wetLevel   = 1.0f;
    reverbParameters.dryLevel   = 0.0f;
    reverbParameters.freezeMode = 0.0f;
    reverb.setParameters (reverbParameters);

    reverb.processStereo (sendBuffer.getWritePointer (0), sendBuffer.getWritePointer (1), numSamples);

    // ---- Master bus: mix in the send, apply gain, then soft clip --------------
    // Freeverb's diffuse tail comes back well below the level of what fed it, so the
    // return needs makeup gain for the mix control to mean anything useful.
    constexpr float reverbReturnGain = 2.0f;

    const float masterGain = dbToGain (globalRefs.master->load());
    const float reverbMix = globalRefs.reverbMix->load() * reverbReturnGain;

    float* mixLeft = mixBuffer.getWritePointer (0);
    float* mixRight = mixBuffer.getWritePointer (1);
    const float* sendLeft = sendBuffer.getReadPointer (0);
    const float* sendRight = sendBuffer.getReadPointer (1);

    for (int i = 0; i < numSamples; ++i)
    {
        mixLeft[i]  = fastTanh ((mixLeft[i]  + sendLeft[i]  * reverbMix) * masterGain);
        mixRight[i] = fastTanh ((mixRight[i] + sendRight[i] * reverbMix) * masterGain);
    }
}

void DrumKitProcessor::handleMidiMessage (const juce::MidiMessage& message)
{
    if (message.isAllNotesOff() || message.isAllSoundOff())
    {
        for (auto& track : tracks)
            track.choke();

        return;
    }

    if (! message.isNoteOn())
        return;

    const int trackIndex = trackForMidiNote (message.getNoteNumber(), getNoteMap());

    if (trackIndex >= 0)
        triggerTrackInternal (trackIndex, message.getFloatVelocity());
}

void DrumKitProcessor::triggerTrackInternal (int trackIndex, float velocity)
{
    if (trackIndex < 0 || trackIndex >= kNumTracks)
        return;

    const float clampedVelocity = juce::jlimit (0.0f, 1.0f, velocity);
    lastVelocity[(size_t) trackIndex].store (clampedVelocity, std::memory_order_relaxed);

    tracks[(size_t) trackIndex].trigger (buildTrackParams (trackIndex, clampedVelocity),
                                         triggerRng.nextU32());

    // Closed and pedal hi-hats cut the open hi-hat short.
    const int group = chokeGroupForTrack (trackIndex);

    if (group != 0)
        for (int other = 0; other < kNumTracks; ++other)
            if (other != trackIndex && chokeGroupForTrack (other) == group)
                tracks[(size_t) other].choke();
}

void DrumKitProcessor::triggerTrack (int trackIndex, float velocity)
{
    if (trackIndex < 0 || trackIndex >= kNumTracks)
        return;

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    triggerFifo.prepareToWrite (1, start1, size1, start2, size2);

    if (size1 > 0)
        triggerQueue[(size_t) start1] = { trackIndex, velocity };
    else if (size2 > 0)
        triggerQueue[(size_t) start2] = { trackIndex, velocity };

    triggerFifo.finishedWrite (size1 + size2);
}

float DrumKitProcessor::readTrackPeak (int trackIndex) noexcept
{
    if (trackIndex < 0 || trackIndex >= kNumTracks)
        return 0.0f;

    return tracks[(size_t) trackIndex].readPeakAndReset();
}

uint32_t DrumKitProcessor::getTrackTriggerCount (int trackIndex) const noexcept
{
    if (trackIndex < 0 || trackIndex >= kNumTracks)
        return 0;

    return tracks[(size_t) trackIndex].getTriggerCount();
}

int DrumKitProcessor::getSelectedModel (int trackIndex) const noexcept
{
    if (trackIndex < 0 || trackIndex >= kNumTracks)
        return 0;

    return (int) std::lround (trackRefs[(size_t) trackIndex].model->load());
}

int DrumKitProcessor::getActiveModel (int trackIndex) const noexcept
{
    if (trackIndex < 0 || trackIndex >= kNumTracks)
        return 0;

    return tracks[(size_t) trackIndex].getModelIndex();
}

ModelParams DrumKitProcessor::buildTrackParams (int trackIndex, float velocity) const
{
    ModulatedValues modulated;
    applyVelocityModulation (modRefs[(size_t) trackIndex], velocity, modulated,
                             sequenceOffsets[(size_t) trackIndex].data());

    const auto value = [&modulated] (ModTarget target)
    {
        return modulated.values[(size_t) (int) target];
    };

    ModelParams params;
    params.level     = dbToGain (value (ModTarget::level));
    params.pan       = value (ModTarget::pan);
    params.tune      = value (ModTarget::tune);
    params.decay     = value (ModTarget::decay);
    params.tone      = value (ModTarget::tone);
    params.character = value (ModTarget::character);
    params.sweep     = value (ModTarget::sweep);
    params.drive     = value (ModTarget::drive);
    params.send      = value (ModTarget::send);
    params.distMode  = (int) std::lround (trackRefs[(size_t) trackIndex].dist->load());

    // Mute and solo are applied after modulation so a muted track stays silent whatever
    // velocity arrives.
    if (blockMuted[(size_t) trackIndex] || (anySolo && ! blockSoloed[(size_t) trackIndex]))
        params.level = 0.0f;

    return params;
}

void DrumKitProcessor::setModulationDepth (int trackIndex, int modTarget, float depth)
{
    modulation.setDepth (trackIndex, modTarget, depth);
}

float DrumKitProcessor::getModulationDepth (int trackIndex, int modTarget) const noexcept
{
    return modulation.getDepth (trackIndex, modTarget);
}

float DrumKitProcessor::getLastVelocity (int trackIndex) const noexcept
{
    if (! juce::isPositiveAndBelow (trackIndex, kNumTracks))
        return kNominalVelocity;

    return lastVelocity[(size_t) trackIndex].load (std::memory_order_relaxed);
}

void DrumKitProcessor::setParameterValue (juce::AudioProcessorValueTreeState& state,
                                          const juce::String& parameterID,
                                          float value)
{
    if (auto* parameter = state.getParameter (parameterID))
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

void DrumKitProcessor::applyModelDefaults (int trackIndex, int modelIndex)
{
    if (trackIndex < 0 || trackIndex >= kNumTracks)
        return;

    const auto& info = getModelInfo (modelIndex);
    const auto id = [trackIndex] (const char* name) { return pid::track (trackIndex, name); };

    setParameterValue (apvts, id (pid::decay),     info.defaultDecay);
    setParameterValue (apvts, id (pid::tone),      info.defaultTone);
    setParameterValue (apvts, id (pid::character), info.defaultCharacter);
    setParameterValue (apvts, id (pid::sweep),     info.defaultSweep);
}

void DrumKitProcessor::applyModelDefaults (int trackIndex)
{
    applyModelDefaults (trackIndex, getSelectedModel (trackIndex));
}

int DrumKitProcessor::getSelectedKit() const noexcept
{
    return (int) std::lround (globalRefs.kit->load());
}

void DrumKitProcessor::applyKitPreset (int kitIndex)
{
    if (! juce::isPositiveAndBelow (kitIndex, numFactoryKits()))
        return;

    setParameterValue (apvts, pid::kit, (float) kitIndex);

    applyKit (*this, factoryKit (kitIndex));
}

juce::AudioProcessorEditor* DrumKitProcessor::createEditor()
{
    return new DrumKitEditor (*this);
}

void DrumKitProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // The lanes, patterns and chain are not parameters, so they ride alongside them in the
    // same tree.
    auto state = apvts.copyState();

    for (const auto* name : { "SEQUENCER", "PATTERNS", "CHAIN" })
        if (const auto existing = state.getChildWithName (juce::Identifier (name)); existing.isValid())
            state.removeChild (existing, nullptr);

    state.appendChild (sequencerStateToValueTree (sequencer), nullptr);
    state.appendChild (patternsToValueTree (*patterns), nullptr);
    state.appendChild (markovChainToValueTree (markovChain), nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void DrumKitProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        if (xml->hasTagName (apvts.state.getType()))
        {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));

            // The depths live in that tree, not in parameters, so re-mirror them.
            modulation.reloadFromState();

            // The lanes too, and a session saved before they existed decodes as defaults.
            sequencerStateFromValueTree (apvts.state.getChildWithName (juce::Identifier ("SEQUENCER")),
                                         sequencer);

            patternsFromValueTree (apvts.state.getChildWithName (juce::Identifier ("PATTERNS")),
                                   *patterns);
            markovChainFromValueTree (apvts.state.getChildWithName (juce::Identifier ("CHAIN")),
                                      markovChain);

            // The chain is audio-thread state; ask the next block to restart it at the
            // position the restored session left the transport at.
            chainNeedsReset.store (true, std::memory_order_relaxed);
            uiGeneration.fetch_add (1, std::memory_order_relaxed);
        }
    }
}

} // namespace df
