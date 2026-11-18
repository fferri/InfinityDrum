#include "MarkovChain.h"

#include <algorithm>
#include <cmath>

namespace df
{

namespace
{
    const juce::Identifier patternsNodeType { "PATTERNS" };
    const juce::Identifier patternNodeType  { "PATTERN" };
    const juce::Identifier sequencerNodeType { "SEQUENCER" };
    const juce::Identifier chainNodeType    { "CHAIN" };
    const juce::Identifier stateNodeType    { "STATE" };
    const juce::Identifier rowNodeType      { "ROW" };
}

//==============================================================================
MarkovChain::MarkovChain()
{
    resetToSequential();
}

void MarkovChain::resetToSequential() noexcept
{
    for (int from = 0; from < kNumChainStates; ++from)
    {
        auto& row = transitions[(size_t) from];
        row.fill (0.0f);
        row[(size_t) ((from + 1) % kNumChainStates)] = 1.0f;
    }
}

void MarkovChain::normaliseAll() noexcept
{
    for (int from = 0; from < kNumChainStates; ++from)
    {
        auto& row = transitions[(size_t) from];

        float sum = 0.0f;

        for (int to = 0; to < kNumChainStates; ++to)
            sum += row[(size_t) to];

        if (sum <= 1.0e-9f)
        {
            row.fill (0.0f);
            row[(size_t) from] = 1.0f;   // a row with no mass stays where it is
            continue;
        }

        for (int to = 0; to < kNumChainStates; ++to)
            row[(size_t) to] /= sum;

        // Put the last bit of floating-point error on the largest cell, so the row is
        // exactly one to within a float rather than merely close.
        int largest = 0;

        for (int to = 1; to < kNumChainStates; ++to)
            if (row[(size_t) to] > row[(size_t) largest])
                largest = to;

        float adjusted = 0.0f;

        for (int to = 0; to < kNumChainStates; ++to)
            adjusted += row[(size_t) to];

        row[(size_t) largest] = std::max (0.0f, row[(size_t) largest] + (1.0f - adjusted));
    }
}

void MarkovChain::setTransition (int from, int to, float value) noexcept
{
    if (from < 0 || from >= kNumChainStates || to < 0 || to >= kNumChainStates)
        return;

    auto& row = transitions[(size_t) from];
    value = std::clamp (value, 0.0f, 1.0f);
    row[(size_t) to] = value;

    if (kNumChainStates == 1)
    {
        row[0] = 1.0f;
        return;
    }

    float others = 0.0f;

    for (int j = 0; j < kNumChainStates; ++j)
        if (j != to)
            others += row[(size_t) j];

    const float remaining = 1.0f - value;

    if (value >= 1.0f)
    {
        for (int j = 0; j < kNumChainStates; ++j)
            if (j != to)
                row[(size_t) j] = 0.0f;
    }
    else if (others > 1.0e-6f)
    {
        // The edited cell keeps exactly the value it was given; the rest of the row shares
        // what is left in the proportions it already had.
        const float scale = remaining / others;

        for (int j = 0; j < kNumChainStates; ++j)
            if (j != to)
                row[(size_t) j] *= scale;
    }
    else
    {
        // Nothing else had any mass, so the remainder goes somewhere it can be heard: the
        // self-transition when the user is editing an outgoing edge, and the next state
        // when they are editing the self-transition itself.
        const int fallback = (to != from) ? from : (from + 1) % kNumChainStates;
        row[(size_t) fallback] = remaining;
    }

    normaliseAll();
}

float MarkovChain::transition (int from, int to) const noexcept
{
    if (from < 0 || from >= kNumChainStates || to < 0 || to >= kNumChainStates)
        return 0.0f;

    return transitions[(size_t) from][(size_t) to];
}

float MarkovChain::rowSum (int from) const noexcept
{
    if (from < 0 || from >= kNumChainStates)
        return 0.0f;

    float sum = 0.0f;

    for (int to = 0; to < kNumChainStates; ++to)
        sum += transitions[(size_t) from][(size_t) to];

    return sum;
}

int MarkovChain::nextState (int from, Rng& rng) const noexcept
{
    if (from < 0 || from >= kNumChainStates)
        return 0;

    const auto& row = transitions[(size_t) from];
    const float draw = rng.nextUnipolar();
    float cumulative = 0.0f;

    for (int to = 0; to < kNumChainStates; ++to)
    {
        cumulative += row[(size_t) to];

        if (draw < cumulative)
            return to;
    }

    // Float error can leave the draw just past the end of a row. Fall back to the last
    // cell with any mass, and to staying put if there is none.
    for (int to = kNumChainStates - 1; to >= 0; --to)
        if (row[(size_t) to] > 0.0f)
            return to;

    return from;
}

//==============================================================================
void ChainPlayer::reset (const MarkovChain& chain, double ppq) noexcept
{
    state = 0;
    stateStartPpq = ppq;
    expectedPpq = ppq;
    phaseOriginPpq = ppq;
    virtualPpqValue = (double) chain.states[0].offsetSteps * kChainStepBeats;
    started = true;
    pattern = nullptr;
    rng.seed (0x2f6e2b1u);
}

void ChainPlayer::advance (const MarkovChain& chain,
                           const std::array<Pattern, (size_t) kNumPatterns>& patterns,
                           const SequencerState& fallback,
                           double ppq, double beatsPerSample, int numSamples, bool playing) noexcept
{
    // A jump in the host position is a seek. Restarting at state zero makes a loop or a
    // seek repeatable instead of letting the chain accumulate a history that depends on
    // how many times the section happened to be played.
    if (! started || std::abs (ppq - expectedPpq) > 1.0e-2)
        reset (chain, ppq);

    bool enteredState = false;

    if (playing)
    {
        // A block can span more than one state boundary if the loops are short; walk them
        // in order so the random draws happen at the boundaries rather than at block rate.
        for (int guard = 0; guard < 4096; ++guard)
        {
            const auto& current = chain.states[(size_t) state];
            const double loopBeats = (double) std::max (1, current.loopSteps) * kChainStepBeats;

            if (ppq < stateStartPpq + loopBeats)
                break;

            stateStartPpq += loopBeats;
            state = chain.nextState (state, rng);
            enteredState = true;
        }
    }

    // The state schedule advances on the exact boundary, but a block cannot change pattern
    // in the middle of itself here. Entering a state at the block start and resetting its
    // phase to the offset means the first step of the new pattern still fires, instead of
    // being skipped because the block began a few milliseconds after the boundary.
    if (enteredState)
        phaseOriginPpq = ppq;

    const auto& current = chain.states[(size_t) state];
    const double offsetBeats = (double) std::max (0, current.offsetSteps) * kChainStepBeats;
    virtualPpqValue = ppq - phaseOriginPpq + offsetBeats;

    const int patternIndex = std::clamp (current.pattern, 0, kNumPatterns - 1);
    const auto& slot = patterns[(size_t) patternIndex];
    pattern = slot.used ? &slot.state : &fallback;

    expectedPpq = playing ? ppq + (double) numSamples * beatsPerSample : ppq;
}

//==============================================================================
juce::ValueTree patternsToValueTree (const std::array<Pattern, (size_t) kNumPatterns>& patterns)
{
    juce::ValueTree root (patternsNodeType);

    for (int i = 0; i < kNumPatterns; ++i)
    {
        const auto& pattern = patterns[(size_t) i];

        // Unused slots are not written. A session that never captured anything therefore
        // carries no pattern payload at all, rather than sixteen copies of the defaults.
        if (! pattern.used)
            continue;

        juce::ValueTree node (patternNodeType);
        node.setProperty ("index", i, nullptr);
        node.appendChild (sequencerStateToValueTree (pattern.state), nullptr);
        root.appendChild (node, nullptr);
    }

    return root;
}

void patternsFromValueTree (const juce::ValueTree& tree,
                            std::array<Pattern, (size_t) kNumPatterns>& patterns)
{
    for (auto& pattern : patterns)
        pattern = Pattern {};

    if (! tree.isValid() || ! tree.hasType (patternsNodeType))
        return;

    for (int i = 0; i < tree.getNumChildren(); ++i)
    {
        const auto node = tree.getChild (i);

        if (! node.hasType (patternNodeType))
            continue;

        const int index = std::clamp ((int) node.getProperty ("index", i), 0, kNumPatterns - 1);
        const auto sequence = node.getChildWithName (sequencerNodeType);

        if (! sequence.isValid())
            continue;

        sequencerStateFromValueTree (sequence, patterns[(size_t) index].state);
        patterns[(size_t) index].used = true;
    }
}

//==============================================================================
juce::ValueTree markovChainToValueTree (const MarkovChain& chain)
{
    juce::ValueTree root (chainNodeType);
    root.setProperty ("enabled", chain.enabled, nullptr);

    for (int i = 0; i < kNumChainStates; ++i)
    {
        const auto& state = chain.states[(size_t) i];

        juce::ValueTree node (stateNodeType);
        node.setProperty ("index", i, nullptr);
        node.setProperty ("pattern", state.pattern, nullptr);
        node.setProperty ("offset", state.offsetSteps, nullptr);
        node.setProperty ("loop", state.loopSteps, nullptr);
        root.appendChild (node, nullptr);

        juce::String values;

        for (int j = 0; j < kNumChainStates; ++j)
        {
            if (j > 0)
                values << ",";

            values << juce::String::formatted ("%.6f", chain.transitions[(size_t) i][(size_t) j]);
        }

        juce::ValueTree row (rowNodeType);
        row.setProperty ("index", i, nullptr);
        row.setProperty ("values", values, nullptr);
        root.appendChild (row, nullptr);
    }

    return root;
}

void markovChainFromValueTree (const juce::ValueTree& tree, MarkovChain& chain)
{
    chain = MarkovChain {};

    if (! tree.isValid() || ! tree.hasType (chainNodeType))
        return;

    chain.enabled = (bool) tree.getProperty ("enabled", false);

    for (int i = 0; i < tree.getNumChildren(); ++i)
    {
        const auto node = tree.getChild (i);

        if (node.hasType (stateNodeType))
        {
            const int index = std::clamp ((int) node.getProperty ("index", i), 0, kNumChainStates - 1);
            auto& state = chain.states[(size_t) index];
            state.pattern = std::clamp ((int) node.getProperty ("pattern", 0), 0, kNumPatterns - 1);
            state.offsetSteps = std::clamp ((int) node.getProperty ("offset", 0),
                                            0, kMaxChainOffsetSteps);
            state.loopSteps = std::clamp ((int) node.getProperty ("loop", 16),
                                          1, kMaxChainLoopSteps);
        }
        else if (node.hasType (rowNodeType))
        {
            const int index = std::clamp ((int) node.getProperty ("index", i), 0, kNumChainStates - 1);

            juce::StringArray values;
            values.addTokens (node.getProperty ("values").toString(), ",", "");

            for (int j = 0; j < kNumChainStates && j < values.size(); ++j)
                chain.transitions[(size_t) index][(size_t) j] =
                    std::clamp (values[(j)].getFloatValue(), 0.0f, 1.0f);
        }
    }

    chain.normaliseAll();
}

} // namespace df
