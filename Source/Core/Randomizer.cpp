#include "Randomizer.h"

namespace df
{

namespace
{

/** Where each engine actually sounds good.

    These are the ranges the twenty factory kits ended up living in, tightened at the ends
    where an engine stops being itself. A kick with decay at 0 is a click and nothing else;
    a kick at 1 is a drone that fights the bassline. Rumble below about 0.4 is not rumble.
*/
struct SweetSpot
{
    float decayLo, decayHi;
    float toneLo, toneHi;
    float characterLo, characterHi;
    float sweepLo, sweepHi;
    float driveLo, driveHi;
    float sendLo, sendHi;
};

constexpr SweetSpot sweetSpots[kNumModels] =
{
    //  decay        tone         character    sweep        drive        send
    { 0.15f, 0.85f, 0.20f, 0.80f, 0.30f, 0.90f, 0.20f, 0.90f, 0.00f, 0.75f, 0.00f, 0.30f },  // kick
    { 0.15f, 0.70f, 0.30f, 0.80f, 0.30f, 0.90f, 0.10f, 0.70f, 0.00f, 0.65f, 0.00f, 0.40f },  // snare
    { 0.20f, 0.90f, 0.20f, 0.80f, 0.20f, 0.90f, 0.10f, 0.60f, 0.00f, 0.55f, 0.00f, 0.45f },  // membrane
    { 0.05f, 0.90f, 0.40f, 0.95f, 0.50f, 1.00f, 0.05f, 0.40f, 0.00f, 0.60f, 0.00f, 0.25f },  // metal
    { 0.10f, 0.70f, 0.30f, 0.80f, 0.20f, 0.80f, 0.30f, 0.90f, 0.00f, 0.65f, 0.00f, 0.40f },  // clap
    { 0.20f, 0.95f, 0.30f, 0.90f, 0.10f, 0.80f, 0.20f, 0.80f, 0.00f, 0.45f, 0.00f, 0.55f },  // cymbal
    { 0.10f, 0.80f, 0.30f, 0.85f, 0.10f, 0.80f, 0.20f, 0.90f, 0.00f, 0.45f, 0.00f, 0.50f },  // fmPerc
    { 0.20f, 0.90f, 0.30f, 0.85f, 0.40f, 1.00f, 0.20f, 0.80f, 0.00f, 0.50f, 0.00f, 0.50f },  // clang
    { 0.40f, 0.95f, 0.10f, 0.55f, 0.30f, 0.90f, 0.30f, 0.90f, 0.00f, 0.60f, 0.00f, 0.40f },  // rumble
};

float between (Rng& rng, float lo, float hi)
{
    return lo + (hi - lo) * rng.nextUnipolar();
}

/** Musical intervals, weighted toward unison.

    Free tuning would beat against the rest of the kit and against whatever the user is
    playing along with. Sticking to octaves, fifths and fourths keeps a randomised machine
    sitting in the key even though nobody chose one.
*/
int randomInterval (Rng& rng)
{
    static constexpr int intervals[] = { 0, 0, 0, 0, 5, 7, 12, -5, -7, -12, 3, -3, 10, -10 };

    return intervals[rng.nextU32() % (uint32_t) (sizeof (intervals) / sizeof (intervals[0]))];
}

} // namespace

namespace
{
    int randomInt (Rng& rng, int low, int high) noexcept
    {
        if (high <= low)
            return low;

        return low + (int) (rng.nextU32() % (uint32_t) (high - low + 1));
    }

    float randomFloat (Rng& rng, float low, float high) noexcept
    {
        return low + rng.nextUnipolar() * (high - low);
    }
}

uint32_t randomSeedForTrack (int trackIndex) noexcept
{
    // Knuth's multiplier scatters adjacent indices well enough for eight of them.
    return (uint32_t) (trackIndex + 1) * 2654435761u;
}

namespace
{
    /** The speed distribution: x1 is half the mass, and each progressively faster or slower
        pair takes half of what is left, so the list reads as "most things are 1/16 notes,
        with occasional doubles, halves, triplets and extremes".

        x1            50%
        x1/2, x2      12.5% each
        x1/3, x3      6.25% each
        x1/4, x4      3.125% each
        x6, x8, x1/8  the remaining 6.25%, split three ways
    */
    struct SpeedWeight
    {
        LaneSpeed speed;
        float weight;
    };

    constexpr float tail = 1.0f / 48.0f;   // 6.25% / 3

    constexpr SpeedWeight speedWeights[]
    {
        { LaneSpeed::xOne,     0.5f },
        { LaneSpeed::xHalf,    0.125f },
        { LaneSpeed::xTwo,     0.125f },
        { LaneSpeed::xThird,   0.0625f },
        { LaneSpeed::xThree,   0.0625f },
        { LaneSpeed::xQuarter, 0.03125f },
        { LaneSpeed::xFour,    0.03125f },
        { LaneSpeed::xEighth,  tail },
        { LaneSpeed::xSix,     tail },
        { LaneSpeed::xEight,   tail },
    };

    LaneSpeed rollSpeed (Rng& rng) noexcept
    {
        const float roll = rng.nextUnipolar();
        float accumulated = 0.0f;

        for (const auto& entry : speedWeights)
        {
            accumulated += entry.weight;

            if (roll < accumulated)
                return entry.speed;
        }

        // Only reachable if the weights sum to just under 1 in floating point.
        return LaneSpeed::xOne;
    }
}

void randomiseTriggerLane (TriggerLane& lane, Rng& rng)
{
    lane.speed = rollSpeed (rng);
    lane.enabled = true;

    if (lane.mode == TriggerMode::euclidean)
    {
        // At least one pulse, or the lane would be silently switched on.
        lane.trigs = randomInt (rng, 1, juce::jmax (1, lane.length));
        lane.offset = randomInt (rng, -lane.length, lane.length);
        return;
    }

    // Roughly a third of the steps, which reads as a pattern rather than a metronome or a
    // wall. Every step gets a velocity whether or not it is on, so switching one on later
    // does not bring back a stale value.
    for (int i = 0; i < kMaxLaneSteps; ++i)
    {
        auto& step = lane.steps[(size_t) i];

        step.on = randomInt (rng, 0, 2) == 0;
        step.velocity = (float) randomInt (rng, 40, 127);
    }

    // Four on the floor is the one thing a roll should not be able to produce by accident,
    // because it looks like the button did nothing.
    bool anyOdd = false;

    for (int i = 0; i < lane.length; ++i)
        anyOdd = anyOdd || ((i % 4) != 0 && lane.steps[(size_t) i].on);

    if (! anyOdd)
        lane.steps[(size_t) (randomInt (rng, 0, juce::jmax (0, lane.length - 1)) % juce::jmax (1, lane.length))]
            .on = true;
}

void randomiseAutomationLane (AutomationLane& lane, Rng& rng)
{
    // Between two and five points: one segment is a straight line, and more than five on a
    // strip this wide is fiddly to grab.
    const int points = randomInt (rng, 2, 5);

    lane.numPoints = points;

    for (int i = 0; i < points; ++i)
    {
        // Evenly spaced with a jitter, then pinned to the ends so the envelope spans the
        // whole cycle instead of leaving dead zones.
        const float spread = (float) i / (float) (points - 1);
        const float jitter = (i == 0 || i == points - 1) ? 0.0f : randomFloat (rng, -0.06f, 0.06f);

        lane.points[(size_t) i].position = juce::jlimit (0.0f, 1.0f, spread + jitter);
        lane.points[(size_t) i].value = randomFloat (rng, -1.0f, 1.0f);
    }

    // Keep them ordered after the jitter.
    for (int i = 1; i < points; ++i)
        lane.points[(size_t) i].position = juce::jmax (lane.points[(size_t) i].position,
                                                        lane.points[(size_t) (i - 1)].position);

    // Sometimes let one interior point land exactly on the one before it. Two points at the
    // same position with different values is a step rather than a ramp, which is what a lot
    // of drum automation actually is - a filter that snaps open, a level that drops. The
    // interpolator returns the later value for a zero-width span, so the jump is exact
    // instead of a very steep slope.
    if (points >= 3 && randomInt (rng, 0, 2) != 0)
    {
        const int index = randomInt (rng, 1, points - 2);

        lane.points[(size_t) index].position = lane.points[(size_t) (index - 1)].position;
    }

    lane.enabled = true;
}

void randomiseTrack (DrumKitProcessor& processor, int trackIndex, Rng& rng)
{
    if (! juce::isPositiveAndBelow (trackIndex, kNumTracks))
        return;

    // Whatever engine the track already has. Never written back, so the host sees no
    // spurious parameter change for it either.
    const int model = juce::jlimit (0, kNumModels - 1, processor.getSelectedModel (trackIndex));
    const auto& spot = sweetSpots[model];

    const auto id = [trackIndex] (const char* name) { return pid::track (trackIndex, name); };

    setParameterValue (processor.apvts, id (pid::dist),
                       (float) (rng.nextU32() % (uint32_t) (int) DistortionMode::count));
    setParameterValue (processor.apvts, id (pid::pan),       between (rng, -0.5f, 0.5f));
    setParameterValue (processor.apvts, id (pid::tune),      (float) randomInterval (rng));
    setParameterValue (processor.apvts, id (pid::decay),     between (rng, spot.decayLo, spot.decayHi));
    setParameterValue (processor.apvts, id (pid::tone),      between (rng, spot.toneLo, spot.toneHi));
    setParameterValue (processor.apvts, id (pid::character), between (rng, spot.characterLo, spot.characterHi));
    setParameterValue (processor.apvts, id (pid::sweep),     between (rng, spot.sweepLo, spot.sweepHi));
    setParameterValue (processor.apvts, id (pid::drive),     between (rng, spot.driveLo, spot.driveHi));
    setParameterValue (processor.apvts, id (pid::send),      between (rng, spot.sendLo, spot.sendHi));

    // ---- velocity response -------------------------------------------------
    // Cleared first, so hitting the button repeatedly gives a fresh machine rather than
    // accumulating whatever the previous draws happened to set.
    for (int i = 0; i < kNumModTargets; ++i)
        processor.setModulationDepth (trackIndex, i, 0.0f);

    processor.setModulationDepth (trackIndex, (int) ModTarget::level,
                                  between (rng, 0.06f, 0.18f));

    if (rng.nextUnipolar() < 0.6f)
        processor.setModulationDepth (trackIndex, (int) ModTarget::character,
                                      between (rng, 0.05f, 0.30f));

    if (rng.nextUnipolar() < 0.4f)
        processor.setModulationDepth (trackIndex, (int) ModTarget::tone,
                                      between (rng, 0.05f, 0.25f));

    if (rng.nextUnipolar() < 0.3f)
        processor.setModulationDepth (trackIndex, (int) ModTarget::decay,
                                      between (rng, 0.05f, 0.20f));
}

} // namespace df
