/*
    InfinityDrum offline verification.

    Renders the processor without a host and asserts the properties that matter:
    every engine makes sound, decays to silence, never produces NaN/Inf, stays inside
    sane level bounds, retriggers without clicking, and honours the GM note map, the
    hi-hat choke group and full state save/restore.

    Exits non-zero if anything fails.
*/

#include <algorithm>
#include <set>
#include <cmath>
#include <iostream>
#include <vector>

#include <juce_events/juce_events.h>

#include "Core/DrumKitProcessor.h"
#include "Core/KitLibrary.h"
#include "Core/Randomizer.h"
#include "Core/Sequencer.h"
#include "Core/Settings.h"
#include "UI/DrumKitEditor.h"

using namespace df;

namespace
{

constexpr double sampleRate = 48000.0;
constexpr int blockSize = 256;

int passed = 0;
int failed = 0;

void check (bool condition, const juce::String& name, const juce::String& detail = {})
{
    std::cout << (condition ? "  PASS  " : "  FAIL  ") << name.toStdString();

    if (detail.isNotEmpty())
        std::cout << "   [" << detail.toStdString() << "]";

    // Flushed immediately, not left to the buffer. Under CI stdout is a pipe, so it is
    // block-buffered: if the suite dies partway through, everything since the last flush
    // disappears - and that is precisely the output that says where it died.
    std::cout << std::endl;

    condition ? ++passed : ++failed;
}

void section (const juce::String& title)
{
    std::cout << "\n" << title.toStdString() << std::endl;
}

void setParameter (DrumKitProcessor& processor, const juce::String& id, float value)
{
    if (auto* parameter = processor.apvts.getParameter (id))
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

float readParameter (DrumKitProcessor& processor, const juce::String& id)
{
    if (auto* value = processor.apvts.getRawParameterValue (id))
        return value->load();

    return std::numeric_limits<float>::quiet_NaN();
}

using Event = std::pair<double, juce::MidiMessage>;

juce::AudioBuffer<float> renderEvents (DrumKitProcessor& processor, const std::vector<Event>& events,
                                       double seconds, double rate = sampleRate)
{
    const int totalSamples = (int) std::ceil (seconds * rate);

    juce::AudioBuffer<float> output (2, totalSamples);
    output.clear();

    juce::AudioBuffer<float> block (2, blockSize);
    juce::MidiBuffer midi;

    for (int start = 0; start < totalSamples; start += blockSize)
    {
        const int numSamples = std::min (blockSize, totalSamples - start);
        block.setSize (2, numSamples, false, false, true);
        midi.clear();

        for (const auto& event : events)
        {
            const int sample = (int) std::llround (event.first * rate);

            if (sample >= start && sample < start + numSamples)
                midi.addEvent (event.second, sample - start);
        }

        processor.processBlock (block, midi);

        for (int channel = 0; channel < 2; ++channel)
            output.copyFrom (channel, start, block, channel, 0, numSamples);
    }

    return output;
}

bool isFinite (const juce::AudioBuffer<float>& buffer)
{
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        const float* samples = buffer.getReadPointer (channel);

        for (int i = 0; i < buffer.getNumSamples(); ++i)
            if (! std::isfinite (samples[i]))
                return false;
    }

    return true;
}

float peakBetween (const juce::AudioBuffer<float>& buffer, double fromSeconds, double toSeconds)
{
    const int from = juce::jlimit (0, buffer.getNumSamples(), (int) (fromSeconds * sampleRate));
    const int to = juce::jlimit (from, buffer.getNumSamples(), (int) (toSeconds * sampleRate));

    if (to <= from)
        return 0.0f;

    return buffer.getMagnitude (from, to - from);
}

float rmsBetween (const juce::AudioBuffer<float>& buffer, double fromSeconds, double toSeconds)
{
    const int from = juce::jlimit (0, buffer.getNumSamples(), (int) (fromSeconds * sampleRate));
    const int to = juce::jlimit (from, buffer.getNumSamples(), (int) (toSeconds * sampleRate));

    if (to <= from)
        return 0.0f;

    return buffer.getRMSLevel (0, from, to - from);
}

float percentile (std::vector<float> values, float fraction)
{
    if (values.empty())
        return 0.0f;

    std::sort (values.begin(), values.end());
    const auto last = (float) (values.size() - 1);
    const auto index = (size_t) juce::jlimit (0.0f, last, fraction * last);
    return values[index];
}

//==============================================================================

void testNoteMap()
{
    section ("GM note map");

    const int expectedNotes[kNumTracks] { 36, 38, 42, 46, 39, 45, 49, 51 };

    for (int t = 0; t < kNumTracks; ++t)
    {
        check (primaryNoteForTrack (t) == expectedNotes[t],
               "track " + juce::String (t + 1) + " primary note",
               juce::String (primaryNoteForTrack (t)));

        check (trackForMidiNote (expectedNotes[t]) == t,
               "track " + juce::String (t + 1) + " resolves from its note");
    }

    check (trackForMidiNote (35) == 0, "note 35 (acoustic bass drum) -> kick");
    check (trackForMidiNote (40) == 1, "note 40 (electric snare) -> snare");
    check (trackForMidiNote (44) == 2, "note 44 (pedal hi-hat) -> closed hat");
    check (trackForMidiNote (47) == 5, "note 47 (low-mid tom) -> membrane");
    check (trackForMidiNote (57) == 6, "note 57 (crash 2) -> cymbal");
    check (trackForMidiNote (59) == 7, "note 59 (ride 2) -> ride");
    check (trackForMidiNote (60) == -1, "note 60 is ignored");
    check (trackForMidiNote (0) == -1, "note 0 is ignored");
}

void testSilence()
{
    section ("Idle output");

    DrumKitProcessor processor;
    processor.prepareToPlay (sampleRate, blockSize);

    const auto audio = renderEvents (processor, {}, 1.0);

    check (isFinite (audio), "idle render is finite");
    check (audio.getMagnitude (0, audio.getNumSamples()) == 0.0f,
           "idle render is digitally silent",
           juce::String (audio.getMagnitude (0, audio.getNumSamples())));
}

void testEveryEngine()
{
    section ("Synthesis engines");

    for (int model = 0; model < kNumModels; ++model)
    {
        const auto& info = getModelInfo (model);
        const juce::String name (info.name);

        DrumKitProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);

        setParameter (processor, pid::track (0, pid::model), (float) model);
        processor.applyModelDefaults (0, model);
        setParameter (processor, pid::reverbMix, 0.0f);
        setParameter (processor, pid::master, 0.0f);

        std::vector<Event> events;
        events.emplace_back (0.05, juce::MidiMessage::noteOn (1, primaryNoteForTrack (0), (juce::uint8) 110));

        const auto audio = renderEvents (processor, events, 9.0);

        check (isFinite (audio), name + ": finite output");

        const float peak = audio.getMagnitude (0, audio.getNumSamples());
        const float peakDb = juce::Decibels::gainToDecibels (peak, -100.0f);

        check (peak > 0.02f, name + ": produces audible output",
               juce::String (peakDb, 1) + " dBFS");

        check (peak <= 1.0001f, name + ": never exceeds full scale",
               juce::String (peakDb, 1) + " dBFS");

        // Decay: the engine must be silent well before the end of the render.
        const float tail = peakBetween (audio, 8.0, 9.0);
        const float tailDb = juce::Decibels::gainToDecibels (tail, -120.0f);

        check (tail < 1.0e-6f, name + ": decays to silence",
               juce::String (tailDb, 1) + " dBFS in the last second");

        // The attack must be prompt: percussive engines should not fade in.
        const float onset = peakBetween (audio, 0.05, 0.08);
        check (onset > 0.01f, name + ": responds within 30 ms",
               juce::String (juce::Decibels::gainToDecibels (onset, -100.0f), 1) + " dBFS");
    }
}

void testSampleRates()
{
    section ("Sample rate independence");

    for (double rate : { 44100.0, 48000.0, 88200.0, 96000.0 })
    {
        DrumKitProcessor processor;
        processor.prepareToPlay (rate, blockSize);
        setParameter (processor, pid::reverbMix, 0.0f);

        std::vector<Event> events;

        for (int t = 0; t < kNumTracks; ++t)
            events.emplace_back (0.05, juce::MidiMessage::noteOn (1, primaryNoteForTrack (t), (juce::uint8) 100));

        const auto audio = renderEvents (processor, events, 4.0, rate);
        const float peak = audio.getMagnitude (0, audio.getNumSamples());

        check (isFinite (audio) && peak > 0.02f && peak <= 1.0001f,
               juce::String ((int) rate) + " Hz renders correctly",
               juce::String (juce::Decibels::gainToDecibels (peak, -100.0f), 1) + " dBFS");
    }
}

void testRetriggerDeclick()
{
    section ("Retrigger de-clicking");

    DrumKitProcessor processor;
    processor.prepareToPlay (sampleRate, blockSize);
    setParameter (processor, pid::reverbMix, 0.0f);
    setParameter (processor, pid::master, 0.0f);

    // A kick with the click removed leaves only low-frequency content, so any sudden
    // step in the waveform stands out clearly as a click.
    setParameter (processor, pid::track (0, pid::tone), 0.0f);
    setParameter (processor, pid::track (0, pid::decay), 0.8f);

    const int note = primaryNoteForTrack (0);
    constexpr double firstHit = 0.20;
    constexpr double secondHit = 0.26;

    std::vector<Event> events;
    events.emplace_back (firstHit, juce::MidiMessage::noteOn (1, note, (juce::uint8) 120));
    events.emplace_back (secondHit, juce::MidiMessage::noteOn (1, note, (juce::uint8) 120));

    const auto audio = renderEvents (processor, events, 1.5);

    check (isFinite (audio), "retriggered kick is finite");

    const float* samples = audio.getReadPointer (0);
    const int retriggerSample = (int) (secondHit * sampleRate);

    std::vector<float> neighbourhood;

    for (int i = retriggerSample - 720; i < retriggerSample + 720; ++i)
        if (i > 0 && i < audio.getNumSamples())
            neighbourhood.push_back (std::abs (samples[i] - samples[i - 1]));

    const float typical = percentile (neighbourhood, 0.90f);
    const float atRetrigger = std::abs (samples[retriggerSample] - samples[retriggerSample - 1]);

    // Where is the biggest jump in the whole render, and is it at the retrigger?
    int worstIndex = 1;
    float worst = 0.0f;

    for (int i = 1; i < audio.getNumSamples(); ++i)
    {
        const float delta = std::abs (samples[i] - samples[i - 1]);

        if (delta > worst)
        {
            worst = delta;
            worstIndex = i;
        }
    }

    std::cout << "        retrigger at sample " << retriggerSample
              << ", largest step anywhere in the render "
              << juce::String (worst, 5).toStdString() << " at sample " << worstIndex << "\n";

    check (atRetrigger <= 3.0f * typical + 1.0e-4f,
           "no step discontinuity at the retrigger point",
           "step " + juce::String (atRetrigger, 5) + " vs p90 " + juce::String (typical, 5));
}

void testHatChoke()
{
    section ("Hi-hat choke group");

    DrumKitProcessor processor;
    processor.prepareToPlay (sampleRate, blockSize);
    setParameter (processor, pid::reverbMix, 0.0f);

    const int openHat = primaryNoteForTrack (3);
    const int closedHat = primaryNoteForTrack (2);

    // Silence the closed hat itself so we only hear the open hat being cut off.
    setParameter (processor, pid::track (2, pid::level), -60.0f);
    setParameter (processor, pid::track (3, pid::decay), 0.85f);

    std::vector<Event> events;
    events.emplace_back (0.05, juce::MidiMessage::noteOn (1, openHat, (juce::uint8) 110));

    const auto control = renderEvents (processor, events, 1.0);
    const float controlLate = rmsBetween (control, 0.40, 0.50);

    check (controlLate > 1.0e-4f, "open hat is still ringing at 400 ms",
           juce::String (juce::Decibels::gainToDecibels (controlLate, -100.0f), 1) + " dBFS");

    std::vector<Event> choked = events;
    choked.emplace_back (0.35, juce::MidiMessage::noteOn (1, closedHat, (juce::uint8) 110));

    const auto audio = renderEvents (processor, choked, 1.0);

    const float before = rmsBetween (audio, 0.30, 0.34);
    const float after = rmsBetween (audio, 0.38, 0.48);

    check (after < before * 0.02f, "a closed hat cuts the open hat short",
           "before " + juce::String (juce::Decibels::gainToDecibels (before, -100.0f), 1)
             + " dB -> after " + juce::String (juce::Decibels::gainToDecibels (after, -100.0f), 1) + " dB");

    check (after < controlLate * 0.05f, "choked tail is far below the unchoked tail",
           juce::String (juce::Decibels::gainToDecibels (after, -100.0f), 1) + " dB vs "
             + juce::String (juce::Decibels::gainToDecibels (controlLate, -100.0f), 1) + " dB");
}

void testFullKitStress()
{
    section ("All eight tracks at once");

    DrumKitProcessor processor;
    processor.prepareToPlay (sampleRate, blockSize);

    std::vector<Event> events;

    // Every track on every sixteenth for four bars, i.e. far more than the kit is
    // meant to play, to make sure the master limiter holds.
    for (int step = 0; step < 64; ++step)
        for (int t = 0; t < kNumTracks; ++t)
            events.emplace_back (step * 0.0625, juce::MidiMessage::noteOn (1, primaryNoteForTrack (t), (juce::uint8) 127));

    const auto audio = renderEvents (processor, events, 4.5);

    check (isFinite (audio), "dense render is finite");

    const float peak = audio.getMagnitude (0, audio.getNumSamples());

    check (peak <= 1.0001f, "master limiter holds full scale",
           juce::String (juce::Decibels::gainToDecibels (peak, -100.0f), 1) + " dBFS");

    check (peak > 0.2f, "dense render is not squashed to nothing",
           juce::String (juce::Decibels::gainToDecibels (peak, -100.0f), 1) + " dBFS");
}

void testModelSwitching()
{
    section ("Live engine switching");

    DrumKitProcessor processor;
    processor.prepareToPlay (sampleRate, blockSize);

    const int note = primaryNoteForTrack (0);

    std::vector<Event> events;

    for (int step = 0; step < 60; ++step)
        events.emplace_back (step * 0.05, juce::MidiMessage::noteOn (1, note, (juce::uint8) 110));

    // Change the engine between hits without re-preparing.
    juce::AudioBuffer<float> block (2, blockSize);
    juce::MidiBuffer midi;

    const int totalSamples = (int) (3.0 * sampleRate);
    juce::AudioBuffer<float> output (2, totalSamples);
    output.clear();

    for (int start = 0; start < totalSamples; start += blockSize)
    {
        const int numSamples = std::min (blockSize, totalSamples - start);

        if (start % (blockSize * 12) == 0)
            setParameter (processor, pid::track (0, pid::model), (float) ((start / (blockSize * 12)) % kNumModels));

        block.setSize (2, numSamples, false, false, true);
        midi.clear();

        for (const auto& event : events)
        {
            const int sample = (int) std::llround (event.first * sampleRate);

            if (sample >= start && sample < start + numSamples)
                midi.addEvent (event.second, sample - start);
        }

        processor.processBlock (block, midi);

        for (int channel = 0; channel < 2; ++channel)
            output.copyFrom (channel, start, block, channel, 0, numSamples);
    }

    check (isFinite (output), "switching engines mid-performance stays finite");
    check (output.getMagnitude (0, output.getNumSamples()) > 0.02f, "still producing sound after switches");
}

void testStateRoundTrip()
{
    section ("State save / restore");

    DrumKitProcessor source;
    source.prepareToPlay (sampleRate, blockSize);

    setParameter (source, pid::track (3, pid::model), (float) ModelId::fmPerc);
    setParameter (source, pid::track (3, pid::decay), 0.731f);
    setParameter (source, pid::track (3, pid::tone), 0.129f);
    setParameter (source, pid::track (5, pid::tune), -7.5f);
    setParameter (source, pid::master, -11.3f);
    setParameter (source, pid::reverbMix, 0.66f);

    juce::MemoryBlock state;
    source.getStateInformation (state);

    check (state.getSize() > 0, "state block is not empty",
           juce::String ((int) state.getSize()) + " bytes");

    DrumKitProcessor restored;
    restored.prepareToPlay (sampleRate, blockSize);
    restored.setStateInformation (state.getData(), (int) state.getSize());

    check (restored.getSelectedModel (3) == (int) ModelId::fmPerc, "model restored on track 4");

    const auto read = [&restored] (const juce::String& id)
    {
        return restored.apvts.getRawParameterValue (id)->load();
    };

    check (std::abs (read (pid::track (3, pid::decay)) - 0.731f) < 1.0e-3f, "decay restored");
    check (std::abs (read (pid::track (3, pid::tone)) - 0.129f) < 1.0e-3f, "tone restored");
    check (std::abs (read (pid::track (5, pid::tune)) + 7.5f) < 1.0e-3f, "tune restored");
    check (std::abs (read (pid::master) + 11.3f) < 1.0e-3f, "master restored",
           juce::String (read (pid::master), 3));
    check (std::abs (read (pid::reverbMix) - 0.66f) < 1.0e-3f, "reverb mix restored");
}

void testUiTriggerAndMetering()
{
    section ("Editor trigger path and metering");

    DrumKitProcessor processor;
    processor.prepareToPlay (sampleRate, blockSize);

    juce::AudioBuffer<float> block (2, blockSize);
    block.clear();
    juce::MidiBuffer midi;

    processor.processBlock (block, midi);

    check (processor.getTrackTriggerCount (0) == 0, "trigger counter starts at zero");

    processor.triggerTrack (0, 1.0f);
    processor.triggerTrack (4, 0.6f);
    processor.triggerTrack (99, 1.0f);   // out of range: must be ignored

    block.clear();
    processor.processBlock (block, midi);

    check (processor.getTrackTriggerCount (0) == 1, "queued pad hit fired track 1");
    check (processor.getTrackTriggerCount (4) == 1, "queued pad hit fired track 5");
    // Read once. readTrackPeak clears as it reads, and argument evaluation order is
    // unspecified in C++, so calling it in both the condition and the detail left the
    // result up to the compiler: clang evaluates the condition first and passes, MSVC
    // evaluates the detail first, consumes the peak, and fails the condition.
    const float firstPeak = processor.readTrackPeak (0);
    check (firstPeak > 0.001f, "track 1 reported a peak level", juce::String (firstPeak, 4));
    check (processor.readTrackPeak (0) == 0.0f, "peak read clears the meter");
}

void testSoloMute()
{
    section ("Mute and solo");

    DrumKitProcessor processor;
    processor.prepareToPlay (sampleRate, blockSize);
    setParameter (processor, pid::reverbMix, 0.0f);

    const int kick = primaryNoteForTrack (0);
    const int snare = primaryNoteForTrack (1);

    std::vector<Event> events;
    events.emplace_back (0.05, juce::MidiMessage::noteOn (1, kick, (juce::uint8) 110));
    events.emplace_back (0.05, juce::MidiMessage::noteOn (1, snare, (juce::uint8) 110));

    const auto both = renderEvents (processor, events, 1.0);
    const float bothRms = rmsBetween (both, 0.05, 0.35);

    // Mute the snare: the kick should remain.
    DrumKitProcessor muted;
    muted.prepareToPlay (sampleRate, blockSize);
    setParameter (muted, pid::reverbMix, 0.0f);
    setParameter (muted, pid::track (1, pid::mute), 1.0f);
    const auto mutedAudio = renderEvents (muted, events, 1.0);
    const float mutedRms = rmsBetween (mutedAudio, 0.05, 0.35);

    check (mutedRms > 0.001f, "muting one track leaves the others audible",
           juce::String (juce::Decibels::gainToDecibels (mutedRms, -100.0f), 1) + " dBFS");
    check (mutedRms < bothRms, "muting reduces the total level",
           juce::String (juce::Decibels::gainToDecibels (mutedRms, -100.0f), 1) + " dB vs "
             + juce::String (juce::Decibels::gainToDecibels (bothRms, -100.0f), 1) + " dB");

    // Solo the snare: the kick must drop out.
    DrumKitProcessor soloed;
    soloed.prepareToPlay (sampleRate, blockSize);
    setParameter (soloed, pid::reverbMix, 0.0f);
    setParameter (soloed, pid::track (1, pid::solo), 1.0f);
    const auto soloAudio = renderEvents (soloed, events, 1.0);

    check (isFinite (soloAudio) && soloAudio.getMagnitude (0, soloAudio.getNumSamples()) > 0.001f,
           "soloing a track still produces sound");
}

void testReverbSend()
{
    section ("Reverb send bus");

    const int kick = primaryNoteForTrack (0);

    std::vector<Event> events;
    events.emplace_back (0.05, juce::MidiMessage::noteOn (1, kick, (juce::uint8) 110));

    const auto describe = [] (float value)
    {
        return juce::String (juce::Decibels::gainToDecibels (value, -120.0f), 1) + " dBFS";
    };

    // Dry reference: the kick itself is long gone by 1.5 s.
    DrumKitProcessor dry;
    dry.prepareToPlay (sampleRate, blockSize);
    setParameter (dry, pid::reverbMix, 0.0f);
    setParameter (dry, pid::track (0, pid::decay), 0.2f);
    const auto dryAudio = renderEvents (dry, events, 2.5);
    const float dryTail = peakBetween (dryAudio, 1.5, 2.5);

    check (dryTail < 1.0e-6f, "dry render has no tail after the hit decays", describe (dryTail));

    // Same hit, but sent to the reverb bus at unity.
    DrumKitProcessor wet;
    wet.prepareToPlay (sampleRate, blockSize);
    setParameter (wet, pid::reverbMix, 1.0f);
    setParameter (wet, pid::reverbSize, 0.9f);
    setParameter (wet, pid::reverbDamp, 0.25f);
    setParameter (wet, pid::track (0, pid::decay), 0.2f);
    setParameter (wet, pid::track (0, pid::send), 1.0f);
    const auto wetAudio = renderEvents (wet, events, 2.5);
    const float wetTail = peakBetween (wetAudio, 1.5, 2.5);

    check (isFinite (wetAudio), "reverb render is finite");
    check (wetTail > 1.0e-4f, "a unity send produces an audible reverb tail", describe (wetTail));
    check (wetTail < 1.0f, "reverb tail stays inside full scale", describe (wetTail));

    // The send control must actually gate the bus.
    DrumKitProcessor noSend;
    noSend.prepareToPlay (sampleRate, blockSize);
    setParameter (noSend, pid::reverbMix, 1.0f);
    setParameter (noSend, pid::track (0, pid::decay), 0.2f);
    setParameter (noSend, pid::track (0, pid::send), 0.0f);
    const auto noSendAudio = renderEvents (noSend, events, 2.5);
    const float noSendTail = peakBetween (noSendAudio, 1.5, 2.5);

    check (noSendTail < 1.0e-6f, "zero send produces no reverb tail", describe (noSendTail));

    // The send is post-fader, so it must scale linearly with the fader rather than with
    // its square. A quadratic send leaves quiet tracks with essentially no reverb, so
    // compare two renders that differ only in the fader position, with the master fixed.
    const auto tailForLevel = [&events] (float levelDb)
    {
        DrumKitProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);
        setParameter (processor, pid::reverbMix, 1.0f);
        setParameter (processor, pid::reverbSize, 0.9f);
        setParameter (processor, pid::reverbDamp, 0.25f);
        setParameter (processor, pid::master, 0.0f);
        setParameter (processor, pid::track (0, pid::decay), 0.2f);
        setParameter (processor, pid::track (0, pid::send), 1.0f);
        setParameter (processor, pid::track (0, pid::level), levelDb);

        return peakBetween (renderEvents (processor, events, 2.5), 1.5, 2.5);
    };

    const float loudTail = tailForLevel (0.0f);
    const float quietTail = tailForLevel (-20.0f);
    const float ratioDb = juce::Decibels::gainToDecibels (quietTail / loudTail, -120.0f);

    // Linear would be -20 dB; a squared send would land near -40 dB.
    check (ratioDb > -24.0f,
           "send follows the fader linearly (post-fader, not fader-squared)",
           "20 dB fader cut cost the tail " + juce::String (ratioDb, 1) + " dB");
}

void testHostLifecycle()
{
    section ("Host lifecycle: prepare / release cycles");

    DrumKitProcessor processor;

    juce::AudioBuffer<float> buffer (2, 1024);
    juce::MidiBuffer midi;

    const auto renderHit = [&] (int numSamples, int note)
    {
        buffer.setSize (2, numSamples, false, false, true);
        buffer.clear();
        midi.clear();
        midi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 110), 0);
        processor.processBlock (buffer, midi);
        return buffer.getMagnitude (0, numSamples);
    };

    const auto audible = [] (float peak) { return std::isfinite (peak) && peak > 0.005f; };

    // Hosts call prepareToPlay more than once - on buffer-size changes, sample-rate
    // changes and device re-activation - and may call releaseResources in between.
    // Replacing the engines used to leave the track's active-engine pointer dangling,
    // so the *second* prepareToPlay dereferenced freed memory and crashed the host.
    processor.prepareToPlay (48000.0, 512);
    check (audible (renderHit (512, 36)), "renders after the first prepareToPlay");

    processor.releaseResources();
    processor.prepareToPlay (48000.0, 256);
    check (audible (renderHit (256, 38)), "renders after releaseResources + prepareToPlay");

    processor.prepareToPlay (44100.0, 1024);
    check (audible (renderHit (1024, 42)), "renders after a sample-rate change");

    processor.prepareToPlay (96000.0, 64);
    check (audible (renderHit (64, 39)), "renders after a block-size change");

    processor.prepareToPlay (48000.0, 128);
    check (audible (renderHit (128, 46)), "renders after returning to 48 kHz");

    // Re-preparing while a voice is still ringing must not use the freed engine.
    renderHit (128, 36);
    processor.prepareToPlay (48000.0, 128);
    check (audible (renderHit (128, 36)), "re-prepare while a hit is still ringing");

    processor.releaseResources();
    processor.releaseResources();
    processor.prepareToPlay (48000.0, 512);
    check (audible (renderHit (512, 36)), "releaseResources is safe to call twice");

    // Hosts do not always hand over exactly the block size they prepared with - some
    // pass a larger buffer, so the renderer has to chunk internally.
    processor.prepareToPlay (48000.0, 64);
    check (audible (renderHit (2048, 36)), "processes a block larger than the prepared size");

    processor.prepareToPlay (48000.0, 4096);
    check (audible (renderHit (32, 38)), "processes a block smaller than the prepared size");

    // Changing the engine between prepares must also be safe.
    setParameter (processor, pid::track (0, pid::model), (float) ModelId::cymbal);
    renderHit (512, 36);
    processor.prepareToPlay (48000.0, 512);
    check (audible (renderHit (512, 36)), "re-prepare after an engine change");

    processor.releaseResources();
}

void testDistortionModes()
{
    section ("Distortion modes");

    const int kick = primaryNoteForTrack (0);

    const auto render = [&] (int mode, float drive)
    {
        DrumKitProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);
        setParameter (processor, pid::reverbMix, 0.0f);
        setParameter (processor, pid::master, 0.0f);
        setParameter (processor, pid::track (0, pid::model), (float) ModelId::kick);
        setParameter (processor, pid::track (0, pid::dist), (float) mode);
        setParameter (processor, pid::track (0, pid::drive), drive);
        setParameter (processor, pid::track (0, pid::level), 0.0f);

        std::vector<Event> events;
        events.emplace_back (0.05, juce::MidiMessage::noteOn (1, kick, (juce::uint8) 110));

        return renderEvents (processor, events, 1.5);
    };

    const auto difference = [] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        const int n = std::min (a.getNumSamples(), b.getNumSamples());

        if (n <= 0)
            return 0.0f;

        double sum = 0.0;

        for (int i = 0; i < n; ++i)
        {
            const double d = (double) a.getReadPointer (0)[i] - (double) b.getReadPointer (0)[i];
            sum += d * d;
        }

        return (float) std::sqrt (sum / n);
    };

    const auto clean = render ((int) DistortionMode::soft, 0.0f);
    const float cleanPeak = clean.getMagnitude (0, clean.getNumSamples());

    check (cleanPeak > 0.01f, "clean reference is audible",
           juce::String (juce::Decibels::gainToDecibels (cleanPeak, -100.0f), 1) + " dBFS");

    // Drive at zero must be a real bypass, whatever curve is selected.
    const auto bypassFold = render ((int) DistortionMode::fold, 0.0f);
    const auto bypassCrush = render ((int) DistortionMode::crush, 0.0f);

    check (difference (bypassFold, bypassCrush) == 0.0f,
           "Drive at 0 is a true bypass regardless of the selected curve");

    const auto softDriven = render ((int) DistortionMode::soft, 1.0f);

    for (int m = 0; m < kNumDistortionModes; ++m)
    {
        const juce::String name (distortionModeName (m));
        const auto driven = render (m, 1.0f);
        const float peak = driven.getMagnitude (0, driven.getNumSamples());

        check (isFinite (driven), name + ": finite output");
        check (peak > 0.01f && peak <= 1.0001f, name + ": audible and inside full scale",
               juce::String (juce::Decibels::gainToDecibels (peak, -100.0f), 1) + " dBFS");

        const float deltaDb = std::abs (juce::Decibels::gainToDecibels (peak / cleanPeak));

        check (deltaDb < 12.0f, name + ": level-compensated against the clean signal",
               juce::String (deltaDb, 1) + " dB difference");

        if ((int) DistortionMode::soft != m)
            check (difference (driven, softDriven) > 1.0e-4f,
                   name + ": produces a different waveform from Soft",
                   "rms difference " + juce::String (difference (driven, softDriven), 5));
    }

    // Harder drive must actually mean more harmonics, not just more gain.
    const auto gentle = render ((int) DistortionMode::fold, 0.15f);
    const auto hard = render ((int) DistortionMode::fold, 1.0f);

    check (difference (gentle, hard) > 1.0e-4f, "Fold responds to the Drive amount",
           "rms difference " + juce::String (difference (gentle, hard), 5));
}

void testKitPresets()
{
    section ("Factory kits");

    check (numKitPresets() >= 3, "three or more kits ship with the plugin",
           juce::String (numKitPresets()) + " kits");

    const auto kitRender = [] (int kitIndex, double seconds)
    {
        DrumKitProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);
        processor.applyKitPreset (kitIndex);

        std::vector<Event> events;

        for (int t = 0; t < kNumTracks; ++t)
            events.emplace_back (0.05 + t * 0.02, juce::MidiMessage::noteOn (1, primaryNoteForTrack (t), (juce::uint8) 110));

        return renderEvents (processor, events, seconds);
    };

    for (int k = 0; k < numKitPresets(); ++k)
    {
        const auto& preset = kitPreset (k);
        const juce::String name (preset.name);

        DrumKitProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);
        processor.applyKitPreset (k);

        bool enginesMatch = true, curvesMatch = true, tuningsMatch = true;

        for (int t = 0; t < kNumTracks; ++t)
        {
            enginesMatch = enginesMatch && processor.getSelectedModel (t) == preset.tracks[t].model;

            const auto read = [&processor, t] (const char* id)
            {
                return processor.apvts.getRawParameterValue (pid::track (t, id))->load();
            };

            curvesMatch = curvesMatch && (int) std::lround (read (pid::dist)) == preset.tracks[t].distMode;
            tuningsMatch = tuningsMatch && std::abs (read (pid::tune) - preset.tracks[t].tune) < 0.02f;
        }

        check (enginesMatch, name + ": engines applied to all eight tracks");
        check (curvesMatch, name + ": distortion curves applied");
        check (tuningsMatch, name + ": tunings applied");

        const auto audio = kitRender (k, 3.0);
        const float peak = audio.getMagnitude (0, audio.getNumSamples());

        check (isFinite (audio) && peak > 0.05f && peak <= 1.0001f,
               name + ": plays a full eight-track hit inside full scale",
               juce::String (juce::Decibels::gainToDecibels (peak, -100.0f), 1) + " dBFS");
    }

    // The twist has to be what you hear first.
    DrumKitProcessor fresh;
    fresh.prepareToPlay (sampleRate, blockSize);

    check (fresh.getSelectedKit() == defaultKit(), "a fresh instance opens on the default kit");
    check (juce::String (kitPreset (defaultKit()).name) == "Industrial",
           "the default kit is Industrial", kitPreset (defaultKit()).name);

    // And Industrial must genuinely differ from the clean Studio kit, not just by name.
    std::vector<Event> events;

    for (int t = 0; t < kNumTracks; ++t)
        events.emplace_back (0.05 + t * 0.02, juce::MidiMessage::noteOn (1, primaryNoteForTrack (t), (juce::uint8) 110));

    const auto renderKit = [&events] (int kitIndex)
    {
        DrumKitProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);
        processor.applyKitPreset (kitIndex);
        return renderEvents (processor, events, 2.0);
    };

    const auto industrial = renderKit (defaultKit());
    const auto studio = renderKit (numKitPresets() - 1);

    const int n = std::min (industrial.getNumSamples(), studio.getNumSamples());
    double sum = 0.0;

    for (int i = 0; i < n; ++i)
    {
        const double d = (double) industrial.getReadPointer (0)[i] - (double) studio.getReadPointer (0)[i];
        sum += d * d;
    }

    const float difference = (float) std::sqrt (sum / juce::jmax (1, n));
    const float reference = studio.getRMSLevel (0, 0, n);

    check (difference > reference * 0.5f,
           "the Industrial kit sounds substantially different from Studio",
           "difference " + juce::String (juce::Decibels::gainToDecibels (difference, -100.0f), 1)
             + " dB vs reference rms " + juce::String (juce::Decibels::gainToDecibels (reference, -100.0f), 1) + " dB");
}

float maxAbsDifference (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    const int n = std::min (a.getNumSamples(), b.getNumSamples());
    float worst = 0.0f;

    for (int i = 0; i < n; ++i)
        worst = std::max (worst, std::abs (a.getReadPointer (0)[i] - b.getReadPointer (0)[i]));

    return worst;
}

void testVelocityModulationMaths()
{
    section ("Velocity modulation: curve and clamping");

    const auto near = [] (float a, float b) { return std::abs (a - b) < 1.0e-5f; };

    check (near (velocityBipolar (0.0f), -1.0f), "velocity 0 maps to -1", juce::String (velocityBipolar (0.0f), 4));
    check (near (velocityBipolar (kNominalVelocity), 0.0f), "velocity 100 maps to 0",
           juce::String (velocityBipolar (kNominalVelocity), 6));
    check (near (velocityBipolar (1.0f), 1.0f), "velocity 127 maps to +1", juce::String (velocityBipolar (1.0f), 4));

    // Symmetric: equal distance above and below the nominal hit.
    const float above = velocityBipolar (1.0f) - velocityBipolar (kNominalVelocity);
    const float below = velocityBipolar (kNominalVelocity) - velocityBipolar (0.0f);
    check (near (above, below), "the sweep is symmetric about the nominal hit",
           "+" + juce::String (above, 4) + " / -" + juce::String (below, 4));

    check (near (velocityBipolar (-3.0f), -1.0f) && near (velocityBipolar (4.0f), 1.0f),
           "velocities outside 0..1 clamp rather than wrap");

    check (velocityBipolar (0.9f) > velocityBipolar (0.8f), "the curve is monotonic");

    // ---- applying it to a value ------------------------------------------
    std::atomic<float> value { 0.5f };
    std::atomic<float> depth { 0.0f };
    juce::NormalisableRange<float> range { 0.0f, 1.0f };

    TrackModRefs refs;

    for (int i = 0; i < kNumModTargets; ++i)
    {
        refs.targets[i].value = &value;
        refs.targets[i].range = &range;
        refs.targets[i].depth = &depth;
    }

    ModulatedValues out;

    for (float v : { 0.0f, 0.4f, kNominalVelocity, 1.0f })
    {
        applyVelocityModulation (refs, v, out);
        check (near (out.values[0], 0.5f), "depth 0 leaves the value untouched at any velocity",
               juce::String (out.values[0], 6));
    }

    depth.store (0.25f);

    applyVelocityModulation (refs, 1.0f, out);
    check (near (out.values[0], 0.75f), "depth +0.25 at full velocity moves a quarter of the travel",
           juce::String (out.values[0], 4));

    applyVelocityModulation (refs, 0.0f, out);
    check (near (out.values[0], 0.25f), "depth +0.25 at zero velocity moves a quarter the other way",
           juce::String (out.values[0], 4));

    applyVelocityModulation (refs, kNominalVelocity, out);
    check (near (out.values[0], 0.5f), "the nominal hit always plays the knob as dialled regardless of depth");

    depth.store (-0.25f);
    applyVelocityModulation (refs, 1.0f, out);
    check (near (out.values[0], 0.25f), "negative depth reverses the direction", juce::String (out.values[0], 4));

    // ---- clamping at the ends --------------------------------------------
    value.store (0.9f);
    depth.store (0.5f);
    applyVelocityModulation (refs, 1.0f, out);
    check (out.values[0] <= 1.0f + 1.0e-5f, "clamps at the top instead of wrapping",
           juce::String (out.values[0], 4));

    value.store (0.1f);
    applyVelocityModulation (refs, 0.0f, out);
    check (out.values[0] >= -1.0e-5f, "clamps at the bottom instead of wrapping",
           juce::String (out.values[0], 4));

    // ---- the band the editor will draw agrees with the audio path ---------
    value.store (0.5f);
    depth.store (0.25f);

    float low = 0.0f, high = 0.0f;

    check (velocityBand (range, 0.5f, 0.25f, low, high), "a modulated knob reports a band");
    check (near (low, 0.25f) && near (high, 0.75f), "the band spans exactly the modulated range",
           juce::String (low, 3) + " .. " + juce::String (high, 3));

    check (! velocityBand (range, 0.5f, 0.0f, low, high), "an unmodulated knob reports no band");

    // A skewed range must not break the symmetry of the band.
    juce::NormalisableRange<float> skewed { 0.0f, 1.0f };
    skewed.setSkewForCentre (0.2f);

    const float centre = skewed.convertTo0to1 (0.5f);

    check (velocityBand (skewed, 0.5f, 0.2f, low, high) && near (high - centre, centre - low),
           "the band stays symmetric about the value on a skewed range",
           "centre " + juce::String (centre, 4) + ", band " + juce::String (low, 4)
             + " .. " + juce::String (high, 4));
}

void testVelocityModulationAudio()
{
    section ("Velocity modulation: through the audio path");

    const auto render = [] (float velocity, float toneDepth, float levelDepth)
    {
        DrumKitProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);
        setParameter (processor, pid::reverbMix, 0.0f);
        setParameter (processor, pid::master, -12.0f);
        setParameter (processor, pid::track (0, pid::level), -12.0f);
        setParameter (processor, pid::track (0, pid::dist), (float) DistortionMode::soft);

        processor.setModulationDepth (0, (int) ModTarget::tone, toneDepth);
        processor.setModulationDepth (0, (int) ModTarget::level, levelDepth);

        std::vector<Event> events;
        events.emplace_back (0.05, juce::MidiMessage::noteOn (1, primaryNoteForTrack (0),
                                                              (juce::uint8) juce::jlimit (1, 127, (int) std::lround (velocity * 127.0f))));

        return renderEvents (processor, events, 1.0);
    };

    // With every depth at zero, velocity is completely inert - a soft hit and a hard hit
    // are bit-identical. This is the property that makes the knob mean what it says.
    const auto softFlat = render (0.2f, 0.0f, 0.0f);
    const auto hardFlat = render (1.0f, 0.0f, 0.0f);

    check (maxAbsDifference (softFlat, hardFlat) <= 0.0f,
           "with every depth at 0, velocity does not change the sound at all");

    // Now give it a tone depth and the timbre starts to track velocity.
    check (maxAbsDifference (render (0.2f, 0.4f, 0.0f), render (1.0f, 0.4f, 0.0f)) > 1.0e-3f,
           "a tone depth makes velocity change the timbre");

    // A level depth must produce an ordered, audible dynamic range.
    const float softPeak = render (0.25f, 0.0f, 0.15f).getMagnitude (0, (int) (sampleRate * 1.0));
    const float midPeak = render (kNominalVelocity, 0.0f, 0.15f).getMagnitude (0, (int) (sampleRate * 1.0));
    const float hardPeak = render (1.0f, 0.0f, 0.15f).getMagnitude (0, (int) (sampleRate * 1.0));

    check (hardPeak > midPeak && midPeak > softPeak, "a level depth makes harder hits louder",
           juce::String (juce::Decibels::gainToDecibels (softPeak, -100.0f), 1) + " / "
             + juce::String (juce::Decibels::gainToDecibels (midPeak, -100.0f), 1) + " / "
             + juce::String (juce::Decibels::gainToDecibels (hardPeak, -100.0f), 1) + " dBFS");

    const float spanDb = juce::Decibels::gainToDecibels (hardPeak / softPeak, -100.0f);

    check (spanDb > 6.0f, "the level depth produces a usable dynamic range",
           juce::String (spanDb, 1) + " dB across velocity 32..127");

    // The nominal hit must land on the knob value whatever the depth is.
    check (std::abs (maxAbsDifference (render (kNominalVelocity, 0.0f, 0.0f),
                                       render (kNominalVelocity, 0.0f, 0.15f))) <= 0.0f,
           "a nominal hit sounds identical whatever the depth, as the design promises");
}

void testModulationStatePersistence()
{
    section ("Velocity modulation: state");

    const auto near = [] (float a, float b) { return std::abs (a - b) < 1.0e-5f; };

    DrumKitProcessor source;
    source.prepareToPlay (sampleRate, blockSize);

    source.setModulationDepth (2, (int) ModTarget::character, 0.42f);
    source.setModulationDepth (2, (int) ModTarget::drive, -0.33f);
    source.setModulationDepth (5, (int) ModTarget::send, 0.75f);

    check (near (source.getModulationDepth (2, (int) ModTarget::character), 0.42f), "a depth reads back");

    source.setModulationDepth (0, (int) ModTarget::decay, 5.0f);
    check (near (source.getModulationDepth (0, (int) ModTarget::decay), 1.0f), "out-of-range depths clamp to +1");

    source.setModulationDepth (0, (int) ModTarget::tone, -9.0f);
    check (near (source.getModulationDepth (0, (int) ModTarget::tone), -1.0f), "and to -1");

    juce::MemoryBlock state;
    source.getStateInformation (state);

    DrumKitProcessor restored;
    restored.prepareToPlay (sampleRate, blockSize);
    restored.setStateInformation (state.getData(), (int) state.getSize());

    check (near (restored.getModulationDepth (2, (int) ModTarget::character), 0.42f), "character depth restored");
    check (near (restored.getModulationDepth (2, (int) ModTarget::drive), -0.33f), "drive depth restored");
    check (near (restored.getModulationDepth (5, (int) ModTarget::send), 0.75f), "send depth restored");
    check (near (restored.getModulationDepth (0, (int) ModTarget::decay), 1.0f), "clamped depth restored");

    // Depths are deliberately not parameters, so they must not show up in automation.
    check (restored.apvts.getParameter ("t3_characterVel") == nullptr
             && restored.apvts.getParameter ("t1_decayMod") == nullptr,
           "depths are not exposed as automatable parameters");

    // A session whose tree has no modulation node - saved before the feature existed, or
    // simply never used - must clear whatever the instance happened to hold.
    DrumKitProcessor legacy;
    legacy.prepareToPlay (sampleRate, blockSize);

    juce::MemoryBlock legacyState;
    legacy.getStateInformation (legacyState);

    DrumKitProcessor reused;
    reused.prepareToPlay (sampleRate, blockSize);
    reused.setModulationDepth (3, (int) ModTarget::tone, 0.9f);
    reused.setStateInformation (legacyState.getData(), (int) legacyState.getSize());

    check (near (reused.getModulationDepth (3, (int) ModTarget::tone), 0.0f),
           "a session with no modulation data clears leftover depths");
}

void testKitDynamics()
{
    section ("Kit velocity dynamics");

    const auto peakAt = [] (int kitIndex, float velocity)
    {
        DrumKitProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);
        processor.applyKitPreset (kitIndex);

        std::vector<Event> events;
        events.emplace_back (0.05, juce::MidiMessage::noteOn (1, primaryNoteForTrack (0),
                                                              (juce::uint8) juce::jlimit (1, 127, (int) std::lround (velocity * 127.0f))));

        return renderEvents (processor, events, 1.0).getMagnitude (0, (int) (sampleRate * 1.0));
    };

    // The engines no longer bake in a velocity response, so if a kit does not dial a level
    // depth then its tracks are completely flat. Every shipped kit must therefore have one.
    for (int k = 0; k < numKitPresets(); ++k)
    {
        const float soft = peakAt (k, 0.25f);
        const float hard = peakAt (k, 1.0f);
        const float spanDb = juce::Decibels::gainToDecibels (hard / soft, -100.0f);

        check (spanDb > 6.0f, juce::String (kitPreset (k).name) + ": velocity produces real dynamics",
               juce::String (spanDb, 1) + " dB between velocity 32 and 127");
    }
}

void testKitLibrary()
{
    section ("Kits: capture, apply, serialise, save");

    const auto near = [] (float a, float b, float tolerance = 1.0e-4f)
    {
        return std::abs (a - b) < tolerance;
    };

    // ---- capture / apply round-trip ---------------------------------------
    DrumKitProcessor source;
    source.prepareToPlay (sampleRate, blockSize);
    source.applyKitPreset (0);
    source.setModulationDepth (3, (int) ModTarget::character, 0.42f);
    source.setModulationDepth (6, (int) ModTarget::pan, -0.25f);
    setParameter (source, pid::track (4, pid::tune), -7.5f);
    setParameter (source, pid::track (2, pid::dist), (float) DistortionMode::fuzz);

    const auto captured = captureKit (source, "Round Trip");
    check (captured.name == "Round Trip", "a captured kit keeps its name");
    check (near (captured.tracks[3].depths[(size_t) (int) ModTarget::character], 0.42f),
           "capture records every modulation depth, not just level");

    DrumKitProcessor restored;
    restored.prepareToPlay (sampleRate, blockSize);
    applyKit (restored, captured);

    check (near (restored.getModulationDepth (3, (int) ModTarget::character), 0.42f),
           "apply restores a character depth");
    check (near (restored.getModulationDepth (6, (int) ModTarget::pan), -0.25f),
           "apply restores a pan depth");
    check (near (readParameter (restored, pid::track (4, pid::tune)), -7.5f, 0.01f),
           "apply restores parameter values",
           juce::String (readParameter (restored, pid::track (4, pid::tune)), 3));
    check ((int) std::lround (readParameter (restored, pid::track (2, pid::dist)))
             == (int) DistortionMode::fuzz,
           "apply restores the distortion curve");

    // ---- a kit defines its modulation completely ---------------------------
    // Loading used to leave any depth the kit did not specify alone. That made sense only
    // while the factory table could carry nothing but a level depth; now a kit is a
    // complete description and partially applying it would silently lose modulation.
    DrumKitProcessor expression;
    expression.prepareToPlay (sampleRate, blockSize);
    expression.setModulationDepth (1, (int) ModTarget::tone, 0.77f);
    applyKit (expression, factoryKit (0));

    check (near (expression.getModulationDepth (1, (int) ModTarget::tone),
                 factoryKit (0).tracks[1].depths[(size_t) (int) ModTarget::tone]),
           "loading a kit replaces the depths rather than merging with yours",
           juce::String (expression.getModulationDepth (1, (int) ModTarget::tone), 3));
    check (expression.getModulationDepth (0, (int) ModTarget::level) > 0.01f,
           "and the kit's own dynamics come through");

    // Every kit, every track, every depth - this is what stops a kit's modulation being
    // quietly dropped on the way into the binary.
    bool allDepthsRoundTrip = true;

    for (int k = 0; k < numFactoryKits(); ++k)
    {
        DrumKitProcessor kitProcessor;
        kitProcessor.prepareToPlay (sampleRate, blockSize);
        kitProcessor.applyKitPreset (k);

        const auto preset = factoryKit (k);

        for (int t = 0; t < kNumTracks; ++t)
            for (int i = 0; i < kNumModTargets; ++i)
                allDepthsRoundTrip = allDepthsRoundTrip
                    && std::abs (kitProcessor.getModulationDepth (t, i)
                                   - preset.tracks[(size_t) t].depths[(size_t) i]) < 1.0e-4f;
    }

    check (allDepthsRoundTrip,
           "every factory kit restores all nine depths on all eight tracks",
           juce::String (numFactoryKits()) + " kits");

    // ---- XML round-trip ----------------------------------------------------
    const auto xml = kitToXml (captured);
    check (xml != nullptr && xml->hasTagName ("DRUMFORGE_KIT"), "a kit serialises to XML");

    KitData reparsed;
    check (kitFromXml (*xml, reparsed), "and parses back");
    check (reparsed.name == captured.name, "the name survives");
    check (near (reparsed.tracks[3].depths[(size_t) (int) ModTarget::character], 0.42f),
           "depths survive serialisation");
    check (near (reparsed.tracks[4].tune, -7.5f, 0.01f), "values survive serialisation");

    juce::XmlElement wrong ("SOMETHING_ELSE");
    KitData ignored;
    check (! kitFromXml (wrong, ignored), "XML from something else is rejected");

    // ---- factory kits are faithfully converted -----------------------------
    for (int k = 0; k < numFactoryKits(); ++k)
    {
        const auto kit = factoryKit (k);
        check (kit.name == kitPreset (k).name && ! kit.name.isEmpty(),
               "factory kit " + juce::String (k) + " keeps its name", kit.name);
        check (kit.tracks[0].model == kitPreset (k).tracks[0].model,
               "factory kit " + juce::String (k) + " keeps its engines");
    }

    // ---- on disk -----------------------------------------------------------
    auto directory = juce::File::getCurrentWorkingDirectory().getChildFile ("build/test-kits");
    directory.deleteRecursively();

    KitLibrary library (directory);
    check (library.getNumKits() == numFactoryKits(),
           "an empty library offers exactly the factory kits",
           juce::String (library.getNumKits()) + " kits");
    check (library.isFactoryKit (0), "the first entries are factory");

    // Guards the generated table: if FactoryKits.inc ever fails to be included, or the
    // emitter writes nothing, the designed kits would vanish silently.
    check (numFactoryKits() >= 23, "the designed kits are compiled in",
           juce::String (numFactoryKits()) + " factory kits");
    check (library.indexOfName ("IN-01 Scrapyard") >= 0, "and reachable by name");
    check (library.indexOfName ("AM-05 Tin Roof Rain") >= 0, "including the last one");
    check (library.deleteKit (0).failed(), "factory kits cannot be deleted");

    auto saved = captured;
    saved.name = "My Kit / 01";          // deliberately awkward as a filename
    check (library.saveKit (saved).wasOk(), "a kit can be saved");
    check (library.getNumKits() == numFactoryKits() + 1, "and shows up in the library");
    check (! library.isFactoryKit (numFactoryKits()), "as a user kit, not a factory one");
    check (library.getName (numFactoryKits()) == "My Kit / 01",
           "under the name it was given, unsanitised", library.getName (numFactoryKits()));

    // A second library over the same directory must find it without being told.
    KitLibrary reopened (directory);
    check (reopened.getNumKits() == numFactoryKits() + 1, "a fresh library finds it on disk");

    const auto loaded = reopened.getKit (numFactoryKits());
    check (near (loaded.tracks[3].depths[(size_t) (int) ModTarget::character], 0.42f),
           "and the reloaded kit still has its depths");
    check (loaded.name == "My Kit / 01", "and its name");

    // Saving under an existing name replaces rather than duplicates.
    auto revised = loaded;
    revised.tracks[0].decay = 0.11f;
    check (reopened.saveKit (revised).wasOk(), "re-saving the same name works");
    check (reopened.getNumKits() == numFactoryKits() + 1, "and replaces rather than duplicates",
           juce::String (reopened.getNumKits()) + " kits");
    check (near (reopened.getKit (numFactoryKits()).tracks[0].decay, 0.11f), "with the new content");

    check (reopened.indexOfName ("My Kit / 01") == numFactoryKits(),
           "a kit can be found by name", juce::String (reopened.indexOfName ("My Kit / 01")));
    check (reopened.indexOfName ("Warehouse") == 1, "including factory kits");
    check (reopened.indexOfName ("No Such Kit") == -1, "and a missing name reports -1");

    check (library.saveKit (KitData {}).failed(), "a kit with no name is refused");
    check (reopened.deleteKit (numFactoryKits()).wasOk(), "a user kit can be deleted");
    check (reopened.getNumKits() == numFactoryKits(), "and is gone");

    directory.deleteRecursively();
}

void testRandomiser()
{
    section ("Randomise: constrained, not chaotic");

    const auto near = [] (float a, float b, float tolerance = 1.0e-4f)
    {
        return std::abs (a - b) < tolerance;
    };

    DrumKitProcessor processor;
    processor.prepareToPlay (sampleRate, blockSize);

    Rng rng { 12345u };

    // ---- what must never move ---------------------------------------------
    setParameter (processor, pid::track (3, pid::level), -12.5f);
    setParameter (processor, pid::track (3, pid::mute), 1.0f);
    setParameter (processor, pid::track (3, pid::solo), 0.0f);

    for (int i = 0; i < 50; ++i)
        randomiseTrack (processor, 3, rng);

    check (near (readParameter (processor, pid::track (3, pid::level)), -12.5f, 0.01f),
           "level survives 50 rolls - it is a mix decision, not a sound one",
           juce::String (readParameter (processor, pid::track (3, pid::level)), 2));
    check (readParameter (processor, pid::track (3, pid::mute)) > 0.5f, "mute survives");
    check (readParameter (processor, pid::track (3, pid::solo)) < 0.5f, "solo survives");

    // ---- the engine is never rerolled --------------------------------------
    // Varying the machine you have is the useful gesture; replacing it with a stranger
    // would discard the choice you made when you picked the track.
    bool enginePreserved = true;
    bool rumbleKeepsLength = true, metalKeepsMetallic = true, kickKeepsClick = true;
    bool allInRange = true;

    for (int model = 0; model < kNumModels; ++model)
    {
        setParameter (processor, pid::track (3, pid::model), (float) model);

        for (int i = 0; i < 60; ++i)
        {
            randomiseTrack (processor, 3, rng);

            const int after = (int) std::lround (readParameter (processor, pid::track (3, pid::model)));
            enginePreserved = enginePreserved && after == model;

            const float decay     = readParameter (processor, pid::track (3, pid::decay));
            const float tone      = readParameter (processor, pid::track (3, pid::tone));
            const float character = readParameter (processor, pid::track (3, pid::character));
            const float sweep     = readParameter (processor, pid::track (3, pid::sweep));
            const float drive     = readParameter (processor, pid::track (3, pid::drive));
            const float send      = readParameter (processor, pid::track (3, pid::send));
            const float pan       = readParameter (processor, pid::track (3, pid::pan));

            // The constraints exist so an engine never rolls a value that stops it being
            // itself. Rumble below ~0.4 is not rumble; metal with its metallic macro low
            // is just noise.
            if (model == (int) ModelId::rumble && decay < 0.35f)    rumbleKeepsLength = false;
            if (model == (int) ModelId::metal && character < 0.45f) metalKeepsMetallic = false;
            if (model == (int) ModelId::kick && sweep < 0.15f)      kickKeepsClick = false;

            for (float v : { decay, tone, character, sweep, drive, send })
                allInRange = allInRange && v >= 0.0f && v <= 1.0f;

            allInRange = allInRange && pan >= -1.0f && pan <= 1.0f;
        }
    }

    check (enginePreserved, "the engine survives 60 rolls on each of the nine",
           juce::String (kNumModels) + " engines x 60 rolls");
    check (rumbleKeepsLength, "rumble never rolls a decay short enough to stop being rumble");
    check (metalKeepsMetallic, "metal never rolls its metallic macro down into noise");
    check (kickKeepsClick, "kick always keeps some sweep, so it still has a click");
    check (allInRange, "every rolled value stays inside its parameter's range");

    // ---- the macros really do move ----------------------------------------
    std::set<float> seenDecays;
    for (int i = 0; i < 40; ++i)
    {
        randomiseTrack (processor, 3, rng);
        seenDecays.insert (readParameter (processor, pid::track (3, pid::decay)));
    }
    check ((int) seenDecays.size() >= 30, "and the parameters actually vary, not just jitter",
           juce::String ((int) seenDecays.size()) + " distinct decays in 40 rolls");

    // Pan is deliberately restrained; a hard-panned machine is disorienting.

    bool panRestrained = true;
    for (int i = 0; i < 200; ++i)
    {
        randomiseTrack (processor, 3, rng);
        panRestrained = panRestrained && std::abs (readParameter (processor, pid::track (3, pid::pan))) <= 0.51f;
    }
    check (panRestrained, "pan stays within a moderate width");

    // ---- determinism -------------------------------------------------------
    const auto roll = [] (uint32_t seed)
    {
        DrumKitProcessor p;
        p.prepareToPlay (sampleRate, blockSize);
        Rng r { seed };
        randomiseTrack (p, 2, r);

        return std::array<float, 6> {
            readParameter (p, pid::track (2, pid::model)),
            readParameter (p, pid::track (2, pid::decay)),
            readParameter (p, pid::track (2, pid::tone)),
            readParameter (p, pid::track (2, pid::character)),
            readParameter (p, pid::track (2, pid::drive)),
            readParameter (p, pid::track (2, pid::send)),
        };
    };

    const auto identical = [] (const std::array<float, 6>& a, const std::array<float, 6>& b)
    {
        for (size_t i = 0; i < a.size(); ++i)
            if (std::abs (a[i] - b[i]) > 1.0e-6f)
                return false;

        return true;
    };

    check (identical (roll (999u), roll (999u)), "the same seed rolls the same machine");
    check (! identical (roll (999u), roll (1000u)), "a different seed rolls a different one");

    // ---- and it makes a sound ---------------------------------------------
    DrumKitProcessor voice;
    voice.prepareToPlay (sampleRate, blockSize);
    setParameter (voice, pid::track (0, pid::level), -6.0f);

    int finite = 0, audible = 0;
    float quietest = 1.0f;

    for (int i = 0; i < 30; ++i)
    {
        randomiseTrack (voice, 0, rng);

        std::vector<Event> events;
        events.emplace_back (0.05, juce::MidiMessage::noteOn (1, primaryNoteForTrack (0),
                                                              (juce::uint8) 110));

        const auto audio = renderEvents (voice, events, 1.0);
        const float peak = audio.getMagnitude (0, audio.getNumSamples());

        if (isFinite (audio)) ++finite;
        if (peak > 0.01f)     ++audible;

        quietest = juce::jmin (quietest, peak);
    }

    check (finite == 30, "every randomised machine renders finite audio",
           juce::String (finite) + "/30");
    check (audible == 30, "and every one of them is audible",
           "quietest " + juce::String (juce::Decibels::gainToDecibels (quietest, -100.0f), 1) + " dBFS");
}

void testNoteMapOptions()
{
    section ("Note map: General MIDI or BeatStep Pro");

    // ---- General MIDI is still the default ---------------------------------
    const int gmNotes[kNumTracks] = { 36, 38, 42, 46, 39, 45, 49, 51 };

    bool gmMatches = true;
    for (int t = 0; t < kNumTracks; ++t)
        gmMatches = gmMatches && primaryNoteForTrack (t) == gmNotes[t];

    check (gmMatches, "the default map is unchanged");

    // ---- the BeatStep Pro numbers ------------------------------------------
    const int bspNotes[kNumTracks] = { 39, 37, 51, 52, 60, 66, 58, 83 };

    bool bspMatches = true;
    for (int t = 0; t < kNumTracks; ++t)
    {
        const int note = primaryNoteForTrack (t, NoteMap::beatStepPro);
        bspMatches = bspMatches && note == bspNotes[t]
                       && trackForMidiNote (note, NoteMap::beatStepPro) == t;
    }

    check (bspMatches, "every BeatStep pad resolves back to its own track");

    // ---- the two maps genuinely disagree -----------------------------------
    // This is why it has to be a switch rather than a union: the same note means two
    // different things, so accepting both at once is impossible.
    check (trackForMidiNote (39, NoteMap::generalMidi) == 4
             && trackForMidiNote (39, NoteMap::beatStepPro) == 0,
           "note 39 is the clap under GM but the kick on a BeatStep");
    check (trackForMidiNote (51, NoteMap::generalMidi) == 7
             && trackForMidiNote (51, NoteMap::beatStepPro) == 2,
           "note 51 is the ride under GM but a hat on a BeatStep");

    // ---- chromatic ----
    bool chromaticMatches = true;

    for (int t = 0; t < kNumTracks; ++t)
        chromaticMatches = chromaticMatches
                            && primaryNoteForTrack (t, NoteMap::chromatic) == 60 + t
                            && trackForMidiNote (60 + t, NoteMap::chromatic) == t;

    check (chromaticMatches, "the chromatic map is eight consecutive notes from middle C",
           juce::String (primaryNoteForTrack (0, NoteMap::chromatic)) + ".."
             + juce::String (primaryNoteForTrack (7, NoteMap::chromatic)));

    check (trackForMidiNote (36, NoteMap::beatStepPro) == -1,
           "the GM kick note does nothing once the BeatStep map is selected");
    check (trackForMidiNote (36, NoteMap::chromatic) == -1,
           "and nothing under the chromatic map either");
    check (trackForMidiNote (83, NoteMap::generalMidi) == -1,
           "and the BeatStep's last pad does nothing under GM");

    // ---- through the processor ---------------------------------------------
    DrumKitProcessor processor;
    processor.prepareToPlay (sampleRate, blockSize);

    check (processor.getNoteMap() == NoteMap::generalMidi,
           "a fresh instance starts on General MIDI");

    const auto triggerWith = [&processor] (int note)
    {
        std::vector<Event> events;
        events.emplace_back (0.0, juce::MidiMessage::noteOn (1, note, (juce::uint8) 110));
        renderEvents (processor, events, 0.2);
    };

    processor.setNoteMap (NoteMap::beatStepPro);
    check (processor.getNoteMap() == NoteMap::beatStepPro, "the map can be changed");

    const auto kickBefore = processor.getTrackTriggerCount (0);
    const auto clapBefore = processor.getTrackTriggerCount (4);

    triggerWith (39);

    check (processor.getTrackTriggerCount (0) == kickBefore + 1,
           "note 39 now triggers the kick");
    check (processor.getTrackTriggerCount (4) == clapBefore,
           "and no longer the clap");

    const auto lastBefore = processor.getTrackTriggerCount (7);
    triggerWith (83);
    check (processor.getTrackTriggerCount (7) == lastBefore + 1,
           "the BeatStep's last pad reaches the last track");

    // The map is a global preference, so a new instance picks it up.
    DrumKitProcessor reopened;
    reopened.prepareToPlay (sampleRate, blockSize);
    check (reopened.getNoteMap() == NoteMap::beatStepPro,
           "a new instance starts on the map last chosen");

    // ---- the map travels with the session ----------------------------------
    // It is a parameter now, so a project that used a BeatStep reopens on the BeatStep
    // map rather than on whatever this machine happens to prefer.
    processor.setNoteMap (NoteMap::beatStepPro);

    juce::MemoryBlock session;
    processor.getStateInformation (session);

    DrumKitProcessor restored;
    restored.prepareToPlay (sampleRate, blockSize);
    restored.setStateInformation (session.getData(), (int) session.getSize());

    check (restored.getNoteMap() == NoteMap::beatStepPro,
           "the map is saved with the session");

    // ---- persistence is self-contained -------------------------------------
    check (juce::String (noteMapName (NoteMap::generalMidi)) == "General MIDI"
             && juce::String (noteMapName (NoteMap::beatStepPro)) == "BeatStep Pro",
           "both maps have names for the menu");

    // A missing or corrupt settings file must fall back rather than misbehave.
    auto scratch = juce::File::getCurrentWorkingDirectory().getChildFile ("build/test-settings-scratch");
    scratch.deleteRecursively();
    scratch.createDirectory();
    scratch.getChildFile ("settings.xml").replaceWithText ("this is not xml");

    check (loadNoteMapSetting (scratch) == NoteMap::generalMidi,
           "a corrupt settings file falls back to General MIDI");

    check (saveNoteMapSetting (NoteMap::beatStepPro, scratch), "settings can be written");
    check (loadNoteMapSetting (scratch) == NoteMap::beatStepPro, "and read back");
    check (saveNoteMapSetting (NoteMap::generalMidi, scratch), "and overwritten");
    check (loadNoteMapSetting (scratch) == NoteMap::generalMidi, "with the new value");

    scratch.deleteRecursively();

    // Leave the suite's own override on General MIDI for any later test.
    processor.setNoteMap (NoteMap::generalMidi);
}

void testSequencerCore()
{
    section ("Sequencer: speeds, Euclidean rhythms, envelopes and sample-accurate triggers");

    // ---- speed is a multiplier of a 1/16 note ----
    check (std::abs (laneSpeedInBeats (LaneSpeed::xOne) - 0.25f) < 1.0e-6f,
           "x1 is a 1/16 note, a quarter of a beat");
    check (std::abs (laneSpeedInBeats (LaneSpeed::xTwo) - 0.125f) < 1.0e-6f, "x2 is a 1/32");
    check (std::abs (laneSpeedInBeats (LaneSpeed::xThird) - 0.75f) < 1.0e-6f,
           "x1/3 is a 3/16", juce::String (laneSpeedInBeats (LaneSpeed::xThird)));
    check (std::abs (laneSpeedInBeats (LaneSpeed::xEighth) - 2.0f) < 1.0e-6f, "x1/8 is a 1/2");
    check (laneSpeedInBeats (LaneSpeed::xEight) < laneSpeedInBeats (LaneSpeed::xEighth),
           "a bigger multiplier is a shorter step");

    // ---- Euclidean ----
    {
        juce::String pattern;

        for (int step = 0; step < 8; ++step)
            pattern << (euclideanStepOn (step, 3, 8) ? "x" : ".");

        check (pattern == "x..x..x.", "E(3,8) is x..x..x.", pattern);
    }

    check (euclideanStepOn (3, 8, 8), "a lane with a pulse on every step is all on");
    check (! euclideanStepOn (0, 0, 8), "zero pulses is silent");

    // ---- the offset rotates the pattern ----
    {
        TriggerLane lane;
        lane.mode = TriggerMode::euclidean;
        lane.length = 8;
        lane.trigs = 3;

        juce::String plain, shifted;

        for (int step = 0; step < 8; ++step)
            plain << (lane.isStepOn (step) ? "x" : ".");

        lane.offset = 2;

        for (int step = 0; step < 8; ++step)
            shifted << (lane.isStepOn (step) ? "x" : ".");

        check (plain == "x..x..x.", "offset 0 leaves E(3,8) alone", plain);
        check (shifted == ".x..x.x.", "an offset of 2 rotates the pattern", shifted);
    }

    {
        const TriggerLane fresh;
        bool anyStepOn = false;

        for (int i = 0; i < kMaxLaneSteps; ++i)
            anyStepOn = anyStepOn || fresh.steps[(size_t) i].on;

        check (! anyStepOn, "a new grid lane starts empty, not with a starter pattern");
    }

    check (TriggerLane().enabled, "a new trigger lane starts enabled");
    check (TriggerLane().speed == LaneSpeed::xOne, "and its speed defaults to x1");

    // ---- per-step velocity ----
    {
        check (std::abs (GridStep {}.velocity - 100.0f) < 1.0e-6f,
               "a new step defaults to velocity 100");

        TriggerLane lane;
        lane.mode = TriggerMode::grid;
        lane.length = 4;
        lane.steps[0].on = true;
        lane.steps[0].velocity = 100.0f;

        check (std::abs (lane.velocityForStep (0) - 100.0f / 127.0f) < 1.0e-5f,
               "a step at velocity 100 fires at 100/127");
        check (lane.velocityForStep (1) == 0.0f, "an off step contributes no velocity");

        lane.steps[0].velocity = 30.0f;
        check (lane.velocityForStep (0) < 100.0f / 127.0f, "a quieter step fires quieter");
    }

    // ---- breakpoint envelope ----
    {
        AutomationLane lane;

        check (std::abs (lane.modulationAt (0.0f) - (-1.0f)) < 1.0e-5f,
               "the default ramp starts at the bottom");
        check (std::abs (lane.modulationAt (0.5f)) < 1.0e-5f, "and crosses zero halfway");
        check (std::abs (lane.modulationAt (1.0f) - (-1.0f)) < 1.0e-5f,
               "while phase 1.0 is phase 0, so it wraps");
    }

    // ---- a trigger carries the sample it belongs at ----
    // beatsPerSample of 0.001 makes 1000 samples exactly one beat, so the expected offsets
    // are readable in the test rather than derived.
    {
        SequencerState state;
        state.triggers[0].mode = TriggerMode::grid;
        state.triggers[0].enabled = true;
        state.triggers[0].speed = LaneSpeed::xQuarter;   // one beat per step
        state.triggers[0].length = 4;

        for (int i = 0; i < 4; ++i)
            state.triggers[0].steps[(size_t) i].on = true;

        SequencerTransport transport;
        SequencerOutput output;

        transport.advance (state, 0.0, 0.001, 1000, true, output);
        check (output.numTriggers == 1 && output.triggers[0].sampleOffset == 0,
               "a boundary at the block start fires at sample 0",
               juce::String (output.numTriggers) + " @ "
                 + juce::String (output.numTriggers > 0 ? output.triggers[0].sampleOffset : -1));

        transport.advance (state, 0.5, 0.001, 1000, true, output);
        check (output.numTriggers == 1 && output.triggers[0].sampleOffset == 500,
               "a boundary inside the block reports its own sample",
               juce::String (output.numTriggers) + " @ "
                 + juce::String (output.numTriggers > 0 ? output.triggers[0].sampleOffset : -1));

        transport.advance (state, 0.9, 0.001, 1000, true, output);
        check (output.numTriggers == 1 && output.triggers[0].sampleOffset == 100,
               "and one already past the start still lands in the right place");

        transport.advance (state, 0.5, 0.001, 1000, false, output);
        check (output.numTriggers == 0, "a stopped transport fires nothing");

        // The real numbers the processor uses: 48 kHz, 120 BPM, a 512-sample block, half a
        // step in. Nothing should fire, and the enumeration must terminate quickly.
        transport.advance (state, 0.125, 120.0 / (60.0 * 48000.0), 512, true, output);
        check (output.numTriggers == 0,
               "halfway between steps at 48 kHz fires nothing",
               juce::String (output.numTriggers));
    }

    // ---- every step in the block, not just the last ----
    {
        SequencerState state;
        state.triggers[0].enabled = true;
        state.triggers[0].speed = LaneSpeed::xTwo;   // a 1/32 note, 0.125 beats
        state.triggers[0].length = 8;

        for (int i = 0; i < 8; ++i)
            state.triggers[0].steps[(size_t) i].on = true;

        SequencerTransport transport;
        SequencerOutput output;

        // One beat of block at 1/32 steps is eight triggers. The old code fired one.
        transport.advance (state, 0.0, 0.001, 1000, true, output);

        check (output.numTriggers == 8,
               "a 1/32 lane fires all eight steps in a one-beat block",
               juce::String (output.numTriggers));

        bool ordered = true;

        for (int i = 1; i < output.numTriggers; ++i)
            ordered = ordered && output.triggers[(size_t) i].sampleOffset
                                    >= output.triggers[(size_t) (i - 1)].sampleOffset;

        check (ordered, "and reports them in sample order");
    }

    // ---- the enable switch, not the speed, decides whether a lane plays ----
    {
        SequencerState state;
        state.triggers[0].length = 4;
        state.triggers[0].enabled = false;

        for (int i = 0; i < 4; ++i)
            state.triggers[0].steps[(size_t) i].on = true;

        SequencerTransport transport;
        SequencerOutput output;
        transport.advance (state, 0.0, 0.001, 1000, true, output);

        check (output.numTriggers == 0, "a disabled lane never fires, whatever its speed");
    }

    // ---- polymeter: different lengths, same speed ----
    {
        SequencerState state;

        for (int t = 0; t < 2; ++t)
        {
            state.triggers[(size_t) t].enabled = true;
            state.triggers[(size_t) t].speed = LaneSpeed::xQuarter;
            state.triggers[(size_t) t].length = t == 0 ? 3 : 4;

            for (int i = 0; i < kMaxLaneSteps; ++i)
                state.triggers[(size_t) t].steps[(size_t) i].on = true;
        }

        SequencerTransport transport;
        SequencerOutput output;

        int hits[2] { 0, 0 };

        for (int beat = 0; beat < 12; ++beat)
        {
            transport.advance (state, (double) beat, 0.001, 1000, true, output);

            for (int i = 0; i < output.numTriggers; ++i)
                ++hits[output.triggers[(size_t) i].track];
        }

        check (hits[0] == hits[1], "two lanes of different length fire equally often",
               juce::String (hits[0]) + " vs " + juce::String (hits[1]));
    }

    // ---- automation is independent of the trigger lane ----
    {
        SequencerState state;
        state.triggers[0].enabled = false;

        auto& levelLane = state.automation[0][(size_t) ModTarget::level];
        levelLane.speed = LaneSpeed::xQuarter;
        levelLane.enabled = true;
        levelLane.length = 4;
        levelLane.amount = 1.0f;

        SequencerTransport transport;
        SequencerOutput output;

        transport.advance (state, 0.0, 0.001, 1000, true, output);
        const float atStart = output.offsets[0][(size_t) ModTarget::level];

        transport.advance (state, 2.0, 0.001, 1000, true, output);
        const float halfway = output.offsets[0][(size_t) ModTarget::level];

        check (output.numTriggers == 0, "the trigger lane is off");
        check (std::abs (atStart - (-1.0f)) < 1.0e-4f, "but the automation lane still runs");
        check (std::abs (halfway) < 1.0e-4f, "and sweeps with its own phase",
               juce::String (halfway));
    }

    // ---- one lane per parameter, and they do not interfere ----
    {
        SequencerState state;

        const auto pinHigh = [] (AutomationLane& lane)
        {
            lane.speed = LaneSpeed::xQuarter;
            lane.enabled = true;
            lane.length = 4;
            lane.amount = 0.5f;
            lane.numPoints = 1;
            lane.points[0] = { 0.0f, 1.0f };
        };

        pinHigh (state.automation[0][(size_t) ModTarget::level]);
        pinHigh (state.automation[0][(size_t) ModTarget::pan]);

        SequencerTransport transport;
        SequencerOutput output;
        transport.advance (state, 0.0, 0.001, 1000, true, output);

        check (std::abs (output.offsets[0][(size_t) ModTarget::level] - 0.5f) < 1.0e-4f,
               "the lane under Level moves level");
        check (std::abs (output.offsets[0][(size_t) ModTarget::pan] - 0.5f) < 1.0e-4f,
               "the lane under Pan moves pan");
        check (output.offsets[0][(size_t) ModTarget::decay] == 0.0f,
               "and a parameter with no lane stays where its knob was put");
    }
}

/** A transport the offline tests can drive. Without it getPlayHead() is null, nothing runs,
    and the sequencer's audio path would be entirely untested. */
class TestPlayHead final : public juce::AudioPlayHead
{
public:
    double ppq = 0.0;
    double bpm = 120.0;
    bool playing = true;

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setPpqPosition (ppq);
        info.setBpm (bpm);
        info.setIsPlaying (playing);
        return info;
    }
};

void testSequencerAudio()
{
    section ("Sequencer: a lane actually plays the kit");

    setSettingsDirectoryOverride (juce::File::getSpecialLocation (juce::File::tempDirectory)
                                      .getChildFile ("InfinityDrumSeqTests"));

    DrumKitProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    auto& state = processor.getSequencerState();
    state.triggers[0].mode = TriggerMode::grid;
    state.triggers[0].enabled = true;
    state.triggers[0].speed = LaneSpeed::xOne;     // a 1/16, a quarter of a beat
    state.triggers[0].length = 4;

    for (int i = 0; i < 4; ++i)
        state.triggers[0].steps[(size_t) i].on = true;

    TestPlayHead playHead;
    processor.setPlayHead (&playHead);

    juce::AudioBuffer<float> buffer (2, 512);

    const auto peakOf = [&buffer, &processor] () -> float
    {
        buffer.clear();
        juce::MidiBuffer midi;
        processor.processBlock (buffer, midi);
        return buffer.getMagnitude (0, buffer.getNumSamples());
    };

    // A step sitting exactly on the transport position now sounds at sample 0. It used to be
    // swallowed by the "do not dump a step on the first block" rule, which only existed
    // because steps were quantised to the block start.
    playHead.ppq = 0.0;
    check (peakOf() > 0.01f, "a step on the transport position sounds immediately");

    // Starting halfway between steps must stay silent: the next boundary is 6000 samples
    // away and the block only renders 512 of them.
    state.triggers[0].enabled = false;

    // Render until the previous hit and its reverb tail have actually gone, rather than for a
    // fixed number of blocks: how long that takes is the engine's business, not the test's.
    for (int i = 0; i < 800; ++i)
        if (peakOf() < 1.0e-4f)
            break;

    state.triggers[0].enabled = true;
    playHead.ppq = 0.125;

    const uint32_t hitsBefore = processor.getTrackTriggerCount (0);
    const float betweenSteps = peakOf();
    const uint32_t hitsAfter = processor.getTrackTriggerCount (0);

    check (hitsAfter == hitsBefore,
           "and starting between steps fires no hit",
           juce::String (hitsAfter - hitsBefore) + " hits, peak " + juce::String (betweenSteps));

    // Back onto a step boundary, then walk forward one step at a time. Starting from 0.125
    // and stepping by 0.25 would stay halfway between boundaries for ever and never fire.
    playHead.ppq = 0.25;

    float loudest = 0.0f;

    for (int i = 0; i < 5; ++i)
    {
        loudest = juce::jmax (loudest, peakOf());
        playHead.ppq += 0.25;
    }

    check (loudest > 0.01f, "a lane triggers audible output once the transport advances",
           juce::String (loudest));

    // Let it decay before checking that a stopped transport is silent.
    state.triggers[0].enabled = false;
    playHead.playing = false;
    playHead.ppq += 1.0;

    for (int i = 0; i < 160; ++i)
        peakOf();

    float afterStop = 0.0f;

    for (int i = 0; i < 4; ++i)
    {
        playHead.ppq += 0.25;
        afterStop = juce::jmax (afterStop, peakOf());
    }

    check (afterStop < 0.01f, "and a stopped transport fires nothing", juce::String (afterStop));

    // An automation lane moves the parameter the next hit is built from.
    {
        auto& lane = state.automation[0][(size_t) ModTarget::level];
        lane.enabled = true;
        lane.speed = LaneSpeed::xOne;
        lane.length = 2;
        lane.amount = 1.0f;
        lane.numPoints = 2;
        lane.points[0] = { 0.0f, -1.0f };
        lane.points[1] = { 1.0f, -1.0f };   // pinned quiet

        state.triggers[0].enabled = true;
        state.triggers[0].speed = LaneSpeed::xOne;
        playHead.playing = true;
        playHead.ppq = 0.0;

        for (int i = 0; i < 160; ++i)
            peakOf();

        playHead.ppq += 0.25;
        const float quiet = peakOf();

        lane.points[0] = { 0.0f, 1.0f };
        lane.points[1] = { 1.0f, 1.0f };    // pinned loud

        playHead.ppq += 0.25;
        const float loud = peakOf();

        check (loud > quiet, "an automation lane moves the parameter the next hit uses",
               juce::String (quiet) + " -> " + juce::String (loud));

        // The playhead the editor draws reads this, so it has to move on its own as the
        // transport advances; it is the only thing proving the position is live rather than
        // being recomputed only when something else redraws the lane.
        const float phaseNow = processor.getAutomationPhase (0, 0);

        playHead.ppq += 0.125;
        peakOf();

        const float phaseLater = processor.getAutomationPhase (0, 0);

        check (std::abs (phaseLater - phaseNow) > 1.0e-3f,
               "the automation phase advances with the transport, for the playhead",
               juce::String (phaseNow) + " -> " + juce::String (phaseLater));
        check (phaseLater >= 0.0f && phaseLater <= 1.0f, "and stays inside its cycle");
    }

    processor.setPlayHead (nullptr);
    processor.releaseResources();
}

void testSequencerPersistence()
{
    section ("Sequencer: lanes survive a save and load");

    setSettingsDirectoryOverride (juce::File::getSpecialLocation (juce::File::tempDirectory)
                                      .getChildFile ("InfinityDrumSeqSaveTests"));

    DrumKitProcessor source;
    auto& lanes = source.getSequencerState();

    lanes.triggers[0].enabled = true;
    lanes.triggers[0].mode = TriggerMode::euclidean;
    lanes.triggers[0].speed = LaneSpeed::xThird;
    lanes.triggers[0].length = 7;
    lanes.triggers[0].trigs = 3;
    lanes.triggers[0].offset = -2;

    lanes.triggers[1].mode = TriggerMode::grid;
    lanes.triggers[1].length = 5;

    for (int i = 0; i < 5; ++i)
        lanes.triggers[1].steps[(size_t) i].on = (i % 2) == 0;

    lanes.triggers[1].steps[2].velocity = 64.0f;

    auto& panic = lanes.automation[0][(size_t) ModTarget::pan];
    panic.enabled = true;
    panic.speed = LaneSpeed::xQuarter;
    panic.length = 3;
    panic.amount = 0.75f;
    panic.numPoints = 3;
    panic.points[0] = { 0.0f, -0.5f };
    panic.points[1] = { 0.4f, 1.0f };
    lanes.automation[0][0].points[2] = { 1.0f, -1.0f };

    juce::MemoryBlock saved;
    source.getStateInformation (saved);

    DrumKitProcessor restored;
    restored.setStateInformation (saved.getData(), (int) saved.getSize());

    const auto& back = restored.getSequencerState();

    check (back.triggers[0].enabled, "the enable switch comes back");
    check (back.triggers[0].mode == TriggerMode::euclidean, "so does the mode");
    check (back.triggers[0].speed == LaneSpeed::xThird, "and the speed");
    check (back.triggers[0].length == 7, "and the length");
    check (back.triggers[0].trigs == 3 && back.triggers[0].offset == -2,
           "and the Euclidean trigs and offset",
           juce::String (back.triggers[0].trigs) + "/" + juce::String (back.triggers[0].offset));

    check (back.triggers[1].length == 5, "a second lane keeps its own length");

    bool stepsMatch = true;

    for (int i = 0; i < 5; ++i)
        stepsMatch = stepsMatch && back.triggers[1].steps[(size_t) i].on == ((i % 2) == 0);

    check (stepsMatch, "and its steps");

    // Per-step velocity travels with the pattern too.
    check (std::abs (back.triggers[1].steps[2].velocity - 64.0f) < 1.0e-3f,
           "a step's velocity comes back with it",
           juce::String (back.triggers[1].steps[2].velocity));

    // The lane is stored under the parameter it automates, so reading it back by that index
    // is itself the check that the target survived.
    const auto& automation = back.automation[0][(size_t) ModTarget::pan];

    check (automation.enabled, "the automation lane comes back enabled, under its parameter");
    check (automation.length == 3, "and its own length");
    check (std::abs (automation.amount - 0.75f) < 1.0e-4f, "and its amount");

    check (automation.numPoints == 3, "and every breakpoint",
           juce::String (automation.numPoints));
    check (std::abs (automation.points[1].position - 0.4f) < 1.0e-3f
             && std::abs (automation.points[1].value - 1.0f) < 1.0e-3f,
           "including their positions and values",
           juce::String (automation.points[1].position) + " @ " + juce::String (automation.points[1].value));

    // A session saved before lanes existed has no node at all, and must decode as defaults
    // rather than keeping whatever the instance happened to be holding.
    {
        SequencerState stale;
        stale.triggers[3].length = 29;
        stale.triggers[3].enabled = false;

        sequencerStateFromValueTree (juce::ValueTree(), stale);

        check (stale.triggers[3].length == 16 && stale.triggers[3].enabled,
               "a session with no lane data falls back to the defaults",
               juce::String (stale.triggers[3].length));
    }
}

void testKitLoadLeavesControlsFree()
{
    section ("Kits: loading one must not lock the controls");

    setSettingsDirectoryOverride (juce::File::getSpecialLocation (juce::File::tempDirectory)
                                      .getChildFile ("InfinityDrumKitLockTests"));

    DrumKitProcessor processor;
    DrumKitEditor editor (processor);

    // Stands in for the host restoring a kit, or the user picking one from the menu: the kit
    // parameter changes, and the editor's timer picks it up and loads the preset.
    processor.applyKitPreset (3);
    editor.refreshFromProcessor();

    const auto tuneID = pid::track (0, pid::tune);
    const float dialled = 7.0f;

    setParameterValue (processor.apvts, tuneID, dialled);

    // The editor's timer runs at 30 Hz. If loading a kit left the polled parameter out of
    // step with what the kit set, every one of these ticks would re-apply the preset and
    // throw the change away - which is exactly what the user saw.
    for (int tick = 0; tick < 10; ++tick)
        editor.refreshFromProcessor();

    const float after = processor.apvts.getRawParameterValue (tuneID)->load();

    check (std::abs (after - dialled) < 1.0e-3f,
           "a knob moved after loading a kit stays where it was put",
           juce::String (after) + " (wanted " + juce::String (dialled) + ")");

    // The chosen engine must survive too, since loading a kit sets it.
    const int model = 2;
    if (auto* parameter = processor.apvts.getParameter (pid::track (0, pid::model)))
        parameter->setValueNotifyingHost (parameter->convertTo0to1 ((float) model));

    for (int tick = 0; tick < 10; ++tick)
        editor.refreshFromProcessor();

    check (processor.getSelectedModel (0) == model,
           "and so does the machine selected after a kit load",
           juce::String (processor.getSelectedModel (0)));
}

void testLaneRandomisers()
{
    section ("Sequencer: the lane dice roll usable lanes");

    Rng rng (0x5eed1234u);

    // ---- trigger lanes ----
    {
        bool enabled = true, lengthKept = true, anyOn = true, anyOff = true, valid = true;
        bool euclideanSane = true;

        for (int roll = 0; roll < 40; ++roll)
        {
            TriggerLane lane;
            lane.length = 12;

            randomiseTriggerLane (lane, rng);

            enabled = enabled && lane.enabled;
            lengthKept = lengthKept && lane.length == 12;

            bool on = false, off = false;

            for (int i = 0; i < lane.length; ++i)
            {
                const auto& step = lane.steps[(size_t) i];
                on = on || step.on;
                off = off || ! step.on;
                valid = valid && step.velocity >= 40.0f && step.velocity <= 127.0f;
            }

            anyOn = anyOn && on;
            anyOff = anyOff && off;

            TriggerLane euclid;
            euclid.mode = TriggerMode::euclidean;
            euclid.length = 12;
            randomiseTriggerLane (euclid, rng);

            euclideanSane = euclideanSane && euclid.enabled
                              && euclid.trigs >= 1 && euclid.trigs <= euclid.length
                              && euclid.offset >= -euclid.length && euclid.offset <= euclid.length;
        }

        check (enabled, "a rolled trigger lane is switched on");
        check (lengthKept, "and keeps its length, so the cycle does not move under you");
        check (anyOn && anyOff, "a rolled grid has both steps on and steps off");
        check (valid, "every rolled velocity is inside 40..127");
        check (euclideanSane, "a rolled Euclidean lane has playable trigs and offset");
    }

    // ---- automation lanes ----
    {
        bool enabled = true, kept = true, shapeOk = true, ordered = true;

        for (int roll = 0; roll < 40; ++roll)
        {
            AutomationLane lane;
            lane.speed = LaneSpeed::xThird;
            lane.length = 7;
            lane.amount = 0.42f;

            randomiseAutomationLane (lane, rng);

            enabled = enabled && lane.enabled;
            kept = kept && lane.speed == LaneSpeed::xThird && lane.length == 7
                        && std::abs (lane.amount - 0.42f) < 1.0e-6f;

            shapeOk = shapeOk && lane.numPoints >= 2 && lane.numPoints <= 5;

            for (int i = 0; i < lane.numPoints; ++i)
            {
                const auto& point = lane.points[(size_t) i];

                shapeOk = shapeOk && point.position >= 0.0f && point.position <= 1.0f
                                    && point.value >= -1.0f && point.value <= 1.0f;

                if (i > 0)
                    ordered = ordered && point.position >= lane.points[(size_t) (i - 1)].position;
            }
        }

        check (enabled, "a rolled automation lane is switched on");
        check (kept, "and keeps its speed, length and amount");
        check (shapeOk, "the envelope has 2..5 points, all inside the strip");
        check (ordered, "with the points kept in order, so interpolation cannot run backwards");
    }

    // ---- two tracks must not roll the same thing ----
    // The lane components seed from their track index. Before they did, every Rng started
    // from the same default and pressing the die once per track filled the page with eight
    // copies of one pattern.
    {
        check (randomSeedForTrack (0) != randomSeedForTrack (1),
               "each track seeds its die differently");

        Rng first (randomSeedForTrack (0)), second (randomSeedForTrack (1));

        TriggerLane a, b;
        a.length = 16;
        b.length = 16;

        randomiseTriggerLane (a, first);
        randomiseTriggerLane (b, second);

        bool different = a.speed != b.speed || a.trigs != b.trigs || a.offset != b.offset;

        for (int i = 0; i < 16; ++i)
            different = different || a.steps[(size_t) i].on != b.steps[(size_t) i].on;

        check (different, "two tracks seeded from their index roll different patterns");
    }

    // ---- the speed distribution is weighted towards x1 ----
    {
        Rng speedRng (0x7777u);

        int speedCounts[kNumLaneSpeeds] {};

        constexpr int totalRolls = 8000;

        for (int i = 0; i < totalRolls; ++i)
        {
            TriggerLane lane;
            randomiseTriggerLane (lane, speedRng);
            ++speedCounts[(int) lane.speed];
        }

        const auto share = [&speedCounts] (LaneSpeed speed)
        {
            return (float) speedCounts[(int) speed] / (float) totalRolls;
        };

        // Generous bands: this is checking the shape of the distribution, not the exact
        // numbers, and a fixed seed makes it deterministic anyway.
        check (std::abs (share (LaneSpeed::xOne) - 0.5f) < 0.03f,
               "x1 takes about half the rolls", juce::String (share (LaneSpeed::xOne)));
        check (std::abs (share (LaneSpeed::xHalf) - 0.125f) < 0.02f
                 && std::abs (share (LaneSpeed::xTwo) - 0.125f) < 0.02f,
               "the x1/2 and x2 pair takes about an eighth each");
        check (std::abs (share (LaneSpeed::xThird) - 0.0625f) < 0.015f
                 && std::abs (share (LaneSpeed::xThree) - 0.0625f) < 0.015f,
               "the triplet pair about a sixteenth each");
        check (share (LaneSpeed::xEighth) > 0.0f && share (LaneSpeed::xEight) > 0.0f
                 && share (LaneSpeed::xSix) > 0.0f,
               "and the extremes still turn up");

        for (int i = 0; i < kNumLaneSpeeds; ++i)
            if ((LaneSpeed) i != LaneSpeed::xOne)
                check (speedCounts[(int) LaneSpeed::xOne] > speedCounts[i],
                       ("x1 is more likely than " + juce::String (laneSpeedName ((LaneSpeed) i)))
                         .toRawUTF8());
    }

    // ---- a rolled envelope can step rather than only ramp ----
    {
        bool anyStep = false;

        for (int roll = 0; roll < 60 && ! anyStep; ++roll)
        {
            Rng r (0x1234u + (uint32_t) roll);
            AutomationLane lane;
            randomiseAutomationLane (lane, r);

            for (int i = 1; i < lane.numPoints; ++i)
                anyStep = anyStep
                            || std::abs (lane.points[(size_t) i].position
                                           - lane.points[(size_t) (i - 1)].position) < 1.0e-6f;
        }

        check (anyStep, "a rolled envelope can contain a step, not just ramps");
    }

    // ---- the automation lane's own default length ----
    check (AutomationLane {}.length == 16, "an unconfigured automation lane spans 16 steps",
           juce::String (AutomationLane {}.length));
}

void testEditorNoteLabels()
{
    section ("Editor: the header note follows the map");

    // This is the wiring the audio tests cannot see. It was wrong once - the refresh
    // landed in the strip's constructor rather than on the editor's timer, so the label
    // was correct on open and then never moved when the map changed.
    DrumKitProcessor processor;
    processor.prepareToPlay (sampleRate, blockSize);

    DrumKitEditor editor (processor);
    editor.setSize (1280, 752);

    processor.setNoteMap (NoteMap::generalMidi);
    editor.refreshFromProcessor();

    check (editor.getTrackNoteLabel (0).contains ("36"), "track 1 shows the General MIDI kick note",
           editor.getTrackNoteLabel (0));
    check (editor.getTrackNoteLabel (2).contains ("42"), "and track 3 the General MIDI hat note",
           editor.getTrackNoteLabel (2));

    processor.setNoteMap (NoteMap::beatStepPro);
    editor.refreshFromProcessor();

    check (editor.getTrackNoteLabel (0).contains ("39"),
           "and follows the map when it is changed while the editor is open",
           editor.getTrackNoteLabel (0));
    check (editor.getTrackNoteLabel (2).contains ("51"), "on every track, not just the first",
           editor.getTrackNoteLabel (2));
    check (! editor.getTrackNoteLabel (0).contains ("36"),
           "with no trace of the old note left behind", editor.getTrackNoteLabel (0));

    // The label also has to be right for an editor opened while a map is already selected.
    processor.setNoteMap (NoteMap::beatStepPro);
    DrumKitEditor reopened (processor);
    reopened.setSize (1280, 752);

    check (reopened.getTrackNoteLabel (7).contains ("83"),
           "an editor opened later starts on the right note", reopened.getTrackNoteLabel (7));

    // ---- pages ----
    // Switching page must not disturb the header: the note follows the map, not the page.
    const auto labelBeforePageSwitch = editor.getTrackNoteLabel (0);

    editor.setMode (EditorUiState::Mode::mod);
    check (editor.getMode() == EditorUiState::Mode::mod, "the editor switches to the modulation page");
    check (editor.getTrackNoteLabel (0) == labelBeforePageSwitch,
           "and the track header still shows the same note there", editor.getTrackNoteLabel (0));

    editor.setMode (EditorUiState::Mode::seq);
    check (editor.getMode() == EditorUiState::Mode::seq, "and to the sequencer page");

    editor.setMode (EditorUiState::Mode::synth);
    check (editor.getMode() == EditorUiState::Mode::synth, "and back to the synth page");

    processor.setNoteMap (NoteMap::generalMidi);
}

} // namespace

int main()
{
    // Nothing in this suite may touch the settings of whoever is running it.
    auto settingsDirectory = juce::File::getCurrentWorkingDirectory()
                                 .getChildFile ("build/test-settings");
    settingsDirectory.deleteRecursively();
    setSettingsDirectoryOverride (settingsDirectory);

    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    std::cout << "InfinityDrum verification suite\n"
              << "============================\n";

    testNoteMap();
    testSilence();
    testEveryEngine();
    testSampleRates();
    testRetriggerDeclick();
    testHatChoke();
    testFullKitStress();
    testModelSwitching();
    testStateRoundTrip();
    testUiTriggerAndMetering();
    testSoloMute();
    testReverbSend();
    testHostLifecycle();
    testDistortionModes();
    testKitPresets();
    testVelocityModulationMaths();
    testVelocityModulationAudio();
    testModulationStatePersistence();
    testKitDynamics();
    testKitLibrary();
    testRandomiser();
    testNoteMapOptions();
    testSequencerCore();
    testSequencerAudio();
    testSequencerPersistence();
    testKitLoadLeavesControlsFree();
    testLaneRandomisers();
    testEditorNoteLabels();

    std::cout << "\n============================\n"
              << passed << " passed, " << failed << " failed\n";

    setSettingsDirectoryOverride ({});
    settingsDirectory.deleteRecursively();

    return failed == 0 ? 0 : 1;
}
