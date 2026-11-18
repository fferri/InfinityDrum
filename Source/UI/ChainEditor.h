#pragma once

#include <array>
#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Core/DrumKitProcessor.h"
#include "SequencerLane.h"
#include "Theme.h"

namespace df
{

/** The transition matrix: one micro-knob per (from, to) pair.

    Drawn as a single component rather than 256 child knobs: the grid is a picture, and a
    child component per cell would only add layout and repaint overhead.
*/
class MarkovMatrixComponent final : public juce::Component,
                                   public juce::SettableTooltipClient
{
public:
    explicit MarkovMatrixComponent (DrumKitProcessor&);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    void setActiveState (int stateIndex);
    void refreshFromProcessor();

private:
    struct Metrics
    {
        float cell = 1.0f, x0 = 0.0f, y0 = 0.0f, label = 22.0f;
    };

    Metrics metrics() const noexcept;
    juce::Rectangle<float> cellBounds (int from, int to) const noexcept;
    bool cellAt (juce::Point<int>, int& from, int& to) const noexcept;
    void setHover (int from, int to);
    void updateTooltip();

    DrumKitProcessor& processor;

    int hoverFrom = -1, hoverTo = -1;
    int dragFrom = -1, dragTo = -1;
    int dragStartY = 0;
    float dragStartValue = 0.0f;
    int activeState = -1;
};

//==============================================================================
/** One row of the state table: state number, pattern, offset and loop length. */
class ChainStateRow final : public juce::Component
{
public:
    ChainStateRow (DrumKitProcessor&, int stateIndex);

    void resized() override;
    void paint (juce::Graphics&) override;

    void refreshFromProcessor();
    void setActive (bool);

private:
    DrumKitProcessor& processor;
    const int stateIndex;

    ValueStepper pattern { "", 1, kNumPatterns };
    ValueStepper offset { "", 0, kMaxChainOffsetSteps };
    ValueStepper loop { "", 1, kMaxChainLoopSteps };

    bool active = false;
};

//==============================================================================
/** The CHAIN page. */
class ChainEditorComponent final : public juce::Component
{
public:
    explicit ChainEditorComponent (DrumKitProcessor&);

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Called from the editor's timer. */
    void refreshFromProcessor();

    /** Called by Load so the editor can switch to the page the pattern is edited on. */
    std::function<void()> onEditRequested;

private:
    void syncControls();
    void updateStatus();
    void updateActiveLabel();

    DrumKitProcessor& processor;

    juce::ToggleButton chainOn { "On" };
    ValueStepper patternSelect { "Slot", 1, kNumPatterns };
    juce::TextButton captureButton { "Capture" }, loadButton { "Load" },
                     sequentialButton { "Sequential" };
    juce::Label status, activeLabel;

    std::array<std::unique_ptr<ChainStateRow>, (size_t) kNumChainStates> rows;
    MarkovMatrixComponent matrix;

    juce::Rectangle<int> topArea, tableArea, tableHeaderArea, hintArea, matrixArea;
    uint32_t lastGeneration = 0;
    int lastActiveState = -1;
};

} // namespace df
