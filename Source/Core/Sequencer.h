#pragma once

#include <array>
#include <cstdint>

#include "Parameters.h"

namespace df
{

/** The speed multiplier for a lane.

    A step is a 1/16 note divided by the multiplier, so **larger is faster**. There is no
    "off" in this list: a lane is disabled by its own switch, so that speed always means
    speed and the control does not do double duty.

    | Multiplier | Step length | At 120 BPM |
    |---|---|---|
    | 1/8  | 1/2 note      | 1000 ms |
    | 1/4  | 1/4 note      |  500 ms |
    | 1/3  | 3/16 note     |  375 ms |
    | 1/2  | 1/8 note      |  250 ms |
    | 1    | 1/16 note     |  125 ms |
    | 2    | 1/32 note     |   62.5 ms |
    | 3    | 1/48 note     |   41.7 ms |
    | 4    | 1/64 note     |   31.25 ms |
    | 6    | 1/96 note     |   20.8 ms |
    | 8    | 1/128 note    |   15.6 ms |
*/
enum class LaneSpeed
{
    xEighth = 0,
    xQuarter,
    xThird,
    xHalf,
    xOne,
    xTwo,
    xThree,
    xFour,
    xSix,
    xEight
};

constexpr int kNumLaneSpeeds = 10;

/** The multiplier itself: 1/8, 1/4, 1/3, ..., 8. */
float laneSpeedMultiplier (LaneSpeed) noexcept;

/** Step duration in beats: (1/16 note) / multiplier = 0.25 / multiplier. */
float laneSpeedInBeats (LaneSpeed) noexcept;

const char* laneSpeedName (LaneSpeed) noexcept;

constexpr int kMaxLaneSteps = 64;
constexpr int kMaxModPoints = 16;

/** How the trigger lane decides which steps fire. */
enum class TriggerMode
{
    grid = 0,   ///< an explicit on/off pattern
    euclidean   ///< pulses spread as evenly as the length allows
};

constexpr int kNumTriggerModes = 2;
const char* triggerModeName (TriggerMode) noexcept;

/** One automation lane per controllable parameter, so a lane's index *is* its target. That
    is what lets the editor show a tab per parameter and know, without asking, which lane
    belongs to which. */
constexpr int kMaxAutomationLanes = (int) kNumModTargets;

/** One breakpoint: position along the cycle 0..1, value -1..+1. */
struct ModPoint
{
    float position = 0.0f;
    float value = 0.0f;
};

//==============================================================================
/** One step of a grid pattern.

    Enabled and velocity are separate: the outline says whether the step fires, the filled
    bar says how hard. A step can be enabled and quiet without being off.
*/
struct GridStep
{
    bool on = false;

    /** 1..127. 100 is the default, which is what most controllers send and what the
        velocity-modulation curve treats as its neutral point. */
    float velocity = 100.0f;
};

//==============================================================================
/** The trigger sequencer for one track.

    Every track has exactly one, and it is either a grid or a Euclidean pattern. Its
    parameters are shared by both modes: speed and length. Turning the speed to zero stops
    it, which is why there is no separate "off" mode.
*/
struct TriggerLane
{
    /** On by default: a lane with no steps is silent anyway, so starting enabled means the
        switch reflects what you intend rather than having to be found first. */
    bool enabled = true;
    TriggerMode mode = TriggerMode::grid;
    LaneSpeed speed = LaneSpeed::xOne;
    int length = 16;

    /** Grid pattern; only the first `length` entries are used. */
    std::array<GridStep, kMaxLaneSteps> steps {};

    /** Euclidean: how many pulses, and where the pattern starts. A negative offset walks the
        pattern backwards, which is the quickest way to get a different feel out of the same
        pulse count. */
    int trigs = 4;
    int offset = 0;

    TriggerLane() noexcept;

    /** Resets the pattern to something playable for the mode, keeping speed and length. */
    void setDefaultsFor (TriggerMode) noexcept;

    /** Is this step on? Handles the wrap and the Euclidean offset. */
    bool isStepOn (int step) const noexcept;

    /** How hard this step fires, 0..1. Euclidean steps share one velocity, because they
        are generated rather than drawn. */
    float velocityForStep (int step) const noexcept;
};

/** Evenly-spread rhythm. This is the closed form that agrees with Bjorklund for the
    evenly-distributed cases, without the recursion. */
bool euclideanStepOn (int step, int pulses, int steps) noexcept;

//==============================================================================
/** One automation lane: a breakpoint envelope sweeping one parameter.

    A track may carry up to kMaxAutomationLanes of these, independently of its trigger
    sequencer. That is the point of splitting them: the *rhythm* and the *movement* are
    separate decisions, with their own lengths and speeds.
*/
struct AutomationLane
{
    bool enabled = false;
    LaneSpeed speed = LaneSpeed::xOne;

    /** Long enough to shape a bar at x1: a four-step envelope at a 1/16 is a quarter of a
        bar, which is too short to be the default. */
    int length = 16;

    float amount = 0.5f; ///< how much of the parameter's travel the envelope reaches

    std::array<ModPoint, kMaxModPoints> points {};
    int numPoints = 2;

    AutomationLane() noexcept;

    /** A ramp, which is the clearest thing to see and to hear as a sweep. */
    void setDefaultShape() noexcept;

    /** The envelope's value at a phase in 0..1, linearly interpolated. */
    float modulationAt (float phase) const noexcept;
};

//==============================================================================
/** Everything the sequencer page and the automation page write. */
struct SequencerState
{
    std::array<TriggerLane, kNumTracks> triggers {};
    std::array<std::array<AutomationLane, kMaxAutomationLanes>, kNumTracks> automation {};
};

/** What the transport produced for one block. */
/** How many triggers one block can carry.

    A block can hold several steps of the same lane - at x8 that is a 1/128 note, 15.6 ms,
    short enough to fit more than once in a large buffer. 64 covers eight lanes firing eight
    times each, well beyond anything musical. */
constexpr int kMaxTriggerEvents = 64;

/** One trigger, positioned within the block so it can be rendered at the right sample. */
struct TriggerEvent
{
    int sampleOffset = 0;
    int track = 0;
    float velocity = 0.0f;
};

struct SequencerOutput
{
    /** Triggers for this block, ordered by sample offset. */
    std::array<TriggerEvent, (size_t) kMaxTriggerEvents> triggers {};
    int numTriggers = 0;

    /** Summed automation offsets per track, in normalised parameter space, -1..+1. */
    std::array<std::array<float, (size_t) kNumModTargets>, (size_t) kNumTracks> offsets {};

    /** Where each lane is in its cycle, 0..1. The editor draws a playhead from this. */
    std::array<float, (size_t) kNumTracks> triggerPhase {};
    std::array<std::array<float, (size_t) kMaxAutomationLanes>, (size_t) kNumTracks> automationPhase {};
};

/** Turns a transport position into triggers and offsets.

    Audio-thread safe: no allocation, no locks, no containers.
*/
/** Serialises every lane into a ValueTree, for the session.
    The element name is stable: it is read back out of a saved project. */
juce::ValueTree sequencerStateToValueTree (const SequencerState&);

/** The inverse. A tree that is missing or unrecognised leaves the state at its defaults,
    which is what an older session - saved before lanes existed - should decode as. */
void sequencerStateFromValueTree (const juce::ValueTree&, SequencerState&);

class SequencerTransport
{
public:
    void reset() noexcept;

    /** `beatsPerSample` converts a distance in beats to a sample offset, and `numSamples` is
        the length of the block being rendered.

        Every step boundary inside the block is reported, not just the last one: a large
        buffer at a fast speed contains more than one, and the earlier version silently
        dropped all but the final step.
    */
    void advance (const SequencerState&, double ppqStart, double beatsPerSample, int numSamples,
                  bool playing, SequencerOutput& out) noexcept;
};

} // namespace df
