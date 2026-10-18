#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "DrumKitProcessor.h"
#include "DspUtils.h"
#include "Sequencer.h"

namespace df
{

/** Randomises one machine's sound, leaving its engine alone.

    The engine is deliberately not rerolled. Choosing an engine is the decision you make
    when you pick a track, and a button that silently replaced it would throw that away -
    the useful gesture is "vary the machine I have", not "replace it with a stranger".

    Deliberately *not* uniform. A uniform draw over nine parameters produces mostly
    unusable results: engines sound good only in particular ranges, sweep at zero removes
    a kick's click entirely, and a freely randomised tuning beats against everything else
    in the kit. So each engine carries a sweet-spot range per macro, tuning moves by
    musical intervals, and pan and send stay modest.

    Level, mute and solo are left alone too. They are mix decisions, and a randomiser that
    moved them would wreck the balance of the kit you are working in - the one thing you
    cannot hear your way back from.
*/
void randomiseTrack (DrumKitProcessor& processor, int trackIndex, Rng& rng);

/** Rolls a lane's speed and pattern, and switches it on.

    The length is left alone: it is the polymeter, and preserving it means a roll changes
    the rhythm inside the cycle rather than the cycle itself. Speed comes from a musical
    subset - the very slow and very fast ends of the range are rarely what you want and
    would mostly produce a lane that is either inaudible or a blur.
*/
/** The seed a lane's die starts from.

    Distinct per track on purpose. Every Rng default-constructs to the same seed, so without
    this each row's die rolled an identical pattern and one press per track filled the page
    with eight copies of the same sequence.
*/
uint32_t randomSeedForTrack (int trackIndex) noexcept;

void randomiseTriggerLane (TriggerLane& lane, Rng& rng);

/** Rolls a fresh breakpoint envelope for the lane, and switches it on.

    Speed, length and amount are left alone: they belong to the lane, and the point of the
    button here is a new shape for the parameter you are already looking at.
*/
void randomiseAutomationLane (AutomationLane& lane, Rng& rng);

} // namespace df
