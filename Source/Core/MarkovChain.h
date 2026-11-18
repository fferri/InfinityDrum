#pragma once

#include <array>
#include <cstdint>

#include <juce_core/juce_core.h>

#include "DspUtils.h"
#include "Sequencer.h"

namespace df
{

/** How many states the chain has, and how many patterns the bank holds.

    Sixteen is a power of two and fills a 16x16 matrix without scrolling. The two counts are
    deliberately equal: a state names a pattern, and a bank larger than the state table would
    only hide material nobody can reach.
*/
constexpr int kNumChainStates = 16;
constexpr int kNumPatterns = 16;

/** The chain counts time in 1/16 notes, as the editor does. */
constexpr double kChainStepBeats = 0.25;

/** Bounds for the state editor. The offset is a phase into the pattern; the loop is how
    long the state lasts before the chain moves on. Both are in 1/16 notes. */
constexpr int kMaxChainOffsetSteps = 1023;
constexpr int kMaxChainLoopSteps = 1024;

/** One Markov state: which pattern to play, where in that pattern to enter, and for how
    long before the next state is chosen. */
struct ChainState
{
    int pattern = 0;      ///< 0..kNumPatterns-1
    int offsetSteps = 0;  ///< 0..kMaxChainOffsetSteps, in 1/16 notes
    int loopSteps = 16;   ///< 1..kMaxChainLoopSteps, in 1/16 notes
};

/** A pattern slot. An unused slot is silent until it is captured from the live sequencer. */
struct Pattern
{
    bool used = false;
    SequencerState state;
};

//==============================================================================
/** A first-order Markov chain over ChainState, plus its transition matrix.

    The matrix is stored as ordinary probabilities: every row sums to one. `setTransition`
    is the only writer the editor uses, and it keeps that invariant while making the edited
    cell follow the value it was given: the rest of the row shares what is left, or the
    self-transition takes it if nothing else has any mass.
*/
class MarkovChain
{
public:
    MarkovChain();

    /** Restores the default matrix: state i goes to (i + 1) % N with probability one. */
    void resetToSequential() noexcept;

    /** Divides any row that does not sum to one. Used after loading a session. */
    void normaliseAll() noexcept;

    /** Sets one cell and renormalises its row. The edited cell keeps `value`. */
    void setTransition (int from, int to, float value) noexcept;

    float transition (int from, int to) const noexcept;
    float rowSum (int from) const noexcept;

    /** Draws the next state from row `from`. A row with no mass stays put. */
    int nextState (int from, Rng& rng) const noexcept;

    std::array<ChainState, (size_t) kNumChainStates> states;

    std::array<std::array<float, (size_t) kNumChainStates>, (size_t) kNumChainStates> transitions;

    bool enabled = false;
};

//==============================================================================
/** The audio-thread playback position of the chain.

    Stateful, unlike SequencerTransport: a Markov chain is path-dependent by definition.
    A seek is detected from the host's ppq and restarts the chain at state zero, so a loop
    or a jump always replays the same sequence from the top rather than growing a private
    history. The pattern itself is still rendered by the stateless transport; this only
    supplies which pattern to render and the phase offset to render it at.
*/
class ChainPlayer
{
public:
    void reset (const MarkovChain&, double ppq) noexcept;

    /** Updates the chain and leaves the active pattern/position available through the
        accessors below. Called once per block while the chain is enabled. */
    void advance (const MarkovChain&,
                  const std::array<Pattern, (size_t) kNumPatterns>&,
                  const SequencerState& fallback,
                  double ppq, double beatsPerSample, int numSamples, bool playing) noexcept;

    int activeState() const noexcept { return state; }
    double virtualPpq() const noexcept { return virtualPpqValue; }
    const SequencerState* activePattern() const noexcept { return pattern; }

private:
    int state = 0;
    double stateStartPpq = 0.0;
    double virtualPpqValue = 0.0;
    double phaseOriginPpq = 0.0;
    double expectedPpq = 0.0;
    bool started = false;
    const SequencerState* pattern = nullptr;
    Rng rng { 0x2f6e2b1u };
};

//==============================================================================
/** Serialisation. A missing tree decodes as unused patterns and the default chain. */
juce::ValueTree patternsToValueTree (const std::array<Pattern, (size_t) kNumPatterns>&);
void patternsFromValueTree (const juce::ValueTree&, std::array<Pattern, (size_t) kNumPatterns>&);

juce::ValueTree markovChainToValueTree (const MarkovChain&);
void markovChainFromValueTree (const juce::ValueTree&, MarkovChain&);

} // namespace df
