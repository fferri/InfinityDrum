#include "ChainEditor.h"

#include <algorithm>
#include <cmath>

namespace df
{

//==============================================================================
MarkovMatrixComponent::MarkovMatrixComponent (DrumKitProcessor& owner)
    : processor (owner)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    updateTooltip();
}

MarkovMatrixComponent::Metrics MarkovMatrixComponent::metrics() const noexcept
{
    const auto area = getLocalBounds().toFloat();

    Metrics m;
    m.label = 22.0f;

    const float availableW = juce::jmax (1.0f, area.getWidth() - m.label);
    const float availableH = juce::jmax (1.0f, area.getHeight() - m.label);

    m.cell = juce::jmin (availableW / (float) kNumChainStates,
                         availableH / (float) kNumChainStates);

    const float gridW = m.cell * (float) kNumChainStates;
    const float gridH = m.cell * (float) kNumChainStates;

    m.x0 = area.getX() + m.label + (availableW - gridW) * 0.5f;
    m.y0 = area.getY() + m.label + (availableH - gridH) * 0.5f;

    return m;
}

juce::Rectangle<float> MarkovMatrixComponent::cellBounds (int from, int to) const noexcept
{
    const auto m = metrics();

    return { m.x0 + (float) to * m.cell,
             m.y0 + (float) from * m.cell,
             m.cell, m.cell };
}

bool MarkovMatrixComponent::cellAt (juce::Point<int> position, int& from, int& to) const noexcept
{
    const auto m = metrics();

    const float fx = ((float) position.x - m.x0) / m.cell;
    const float fy = ((float) position.y - m.y0) / m.cell;

    if (fx < 0.0f || fy < 0.0f)
        return false;

    from = (int) std::floor (fy);
    to = (int) std::floor (fx);

    return juce::isPositiveAndBelow (from, kNumChainStates)
             && juce::isPositiveAndBelow (to, kNumChainStates);
}

void MarkovMatrixComponent::setHover (int from, int to)
{
    if (from == hoverFrom && to == hoverTo)
        return;

    hoverFrom = from;
    hoverTo = to;
    updateTooltip();
    repaint();
}

void MarkovMatrixComponent::updateTooltip()
{
    if (hoverFrom >= 0 && hoverTo >= 0)
    {
        const float value = processor.getMarkovChain().transition (hoverFrom, hoverTo);

        setTooltip ("State " + juce::String (hoverFrom + 1) + " -> "
                      + juce::String (hoverTo + 1) + ": "
                      + juce::String (juce::roundToInt (value * 100.0f)) + "%\n"
                      + "Drag vertically to change; right-click to zero.");
    }
    else
    {
        setTooltip ("Rows are the state being left; columns are the destination.");
    }
}

void MarkovMatrixComponent::paint (juce::Graphics& g)
{
    const auto m = metrics();
    const auto& chain = processor.getMarkovChain();

    // ---- Row and column labels ------------------------------------------------
    g.setFont (theme::font (9.0f));

    for (int to = 0; to < kNumChainStates; ++to)
    {
        g.setColour (to == hoverTo ? theme::accent : theme::textDim);
        g.drawText (juce::String (to + 1),
                    juce::Rectangle<int> ((int) (m.x0 + (float) to * m.cell),
                                          (int) (m.y0 - m.label),
                                          (int) m.cell,
                                          (int) m.label - 2),
                    juce::Justification::centred, false);
    }

    for (int from = 0; from < kNumChainStates; ++from)
    {
        g.setColour (from == activeState ? theme::accent : theme::textDim);
        g.drawText (juce::String (from + 1),
                    juce::Rectangle<int> ((int) (m.x0 - m.label),
                                          (int) (m.y0 + (float) from * m.cell),
                                          (int) m.label - 4,
                                          (int) m.cell),
                    juce::Justification::centredRight, false);
    }

    // A faint band under the row the chain is currently in, so the matrix doubles as a
    // live position display while the host is playing.
    if (activeState >= 0)
    {
        g.setColour (theme::accent.withAlpha (0.055f));
        g.fillRoundedRectangle (m.x0,
                                m.y0 + (float) activeState * m.cell,
                                (float) kNumChainStates * m.cell,
                                m.cell,
                                4.0f);
    }

    // ---- Cells ----------------------------------------------------------------
    for (int from = 0; from < kNumChainStates; ++from)
    {
        for (int to = 0; to < kNumChainStates; ++to)
        {
            const auto cell = cellBounds (from, to);
            const auto circle = cell.reduced (m.cell * 0.16f);

            g.setColour (theme::panelAlt);
            g.fillEllipse (circle);
            g.setColour (theme::outline);
            g.drawEllipse (circle, 1.0f);

            const float value = chain.transition (from, to);

            if (value > 0.0005f)
            {
                const auto centre = circle.getCentre();
                const float radius = juce::jmax (2.0f, circle.getWidth() * 0.5f - 2.0f);

                g.setColour (theme::velocitySoft.withAlpha (0.95f));

                if (value >= 0.999f)
                {
                    g.drawEllipse (circle.reduced (2.0f), 2.0f);
                }
                else
                {
                    juce::Path arc;
                    arc.addCentredArc (centre.x, centre.y, radius, radius, 0.0f,
                                       -juce::MathConstants<float>::halfPi,
                                       -juce::MathConstants<float>::halfPi
                                           + juce::MathConstants<float>::twoPi * value,
                                       true);

                    g.strokePath (arc, juce::PathStrokeType (2.0f,
                                                            juce::PathStrokeType::curved,
                                                            juce::PathStrokeType::rounded));

                    const float angle = -juce::MathConstants<float>::halfPi
                                          + juce::MathConstants<float>::twoPi * value;
                    const auto tip = centre
                                       + juce::Point<float> (std::cos (angle), std::sin (angle))
                                             * radius;
                    g.drawLine (centre.x, centre.y, tip.x, tip.y, 1.2f);
                }

                g.fillEllipse (centre.x - 1.5f, centre.y - 1.5f, 3.0f, 3.0f);
            }

            if ((from == hoverFrom && to == hoverTo) || (from == dragFrom && to == dragTo))
            {
                g.setColour (theme::text);
                g.drawEllipse (circle.expanded (1.5f), 1.2f);
            }
        }
    }
}

void MarkovMatrixComponent::mouseDown (const juce::MouseEvent& event)
{
    int from = -1, to = -1;

    if (! cellAt (event.getPosition(), from, to))
        return;

    if (event.mods.isPopupMenu())
    {
        processor.getMarkovChain().setTransition (from, to, 0.0f);
        updateTooltip();
        repaint();
        return;
    }

    dragFrom = from;
    dragTo = to;
    dragStartY = event.y;
    dragStartValue = processor.getMarkovChain().transition (from, to);

    setHover (from, to);
}

void MarkovMatrixComponent::mouseDrag (const juce::MouseEvent& event)
{
    if (dragFrom < 0 || dragTo < 0)
        return;

    const float delta = (float) (dragStartY - event.y) / 100.0f;
    processor.getMarkovChain().setTransition (dragFrom, dragTo, dragStartValue + delta);
    updateTooltip();
    repaint();
}

void MarkovMatrixComponent::mouseUp (const juce::MouseEvent&)
{
    dragFrom = -1;
    dragTo = -1;
}

void MarkovMatrixComponent::mouseMove (const juce::MouseEvent& event)
{
    int from = -1, to = -1;

    if (cellAt (event.getPosition(), from, to))
        setHover (from, to);
    else
        setHover (-1, -1);
}

void MarkovMatrixComponent::mouseExit (const juce::MouseEvent&)
{
    setHover (-1, -1);
}

void MarkovMatrixComponent::setActiveState (int stateIndex)
{
    if (stateIndex == activeState)
        return;

    activeState = stateIndex;
    repaint();
}

void MarkovMatrixComponent::refreshFromProcessor()
{
    setActiveState (processor.getChainActiveState());
}

//==============================================================================
ChainStateRow::ChainStateRow (DrumKitProcessor& owner, int index)
    : processor (owner), stateIndex (index)
{
    for (auto* stepper : { &pattern, &offset, &loop })
        addAndMakeVisible (*stepper);

    pattern.setTooltip ("Pattern played in this state (1-16).");
    offset.setTooltip ("Where in the pattern this state starts, in 1/16 notes.");
    loop.setTooltip ("How long this state lasts before the chain moves on, in 1/16 notes.");

    pattern.onChange = [this] (int value)
    {
        processor.getMarkovChain().states[(size_t) stateIndex].pattern = value - 1;
        repaint();
    };

    offset.onChange = [this] (int value)
    {
        processor.getMarkovChain().states[(size_t) stateIndex].offsetSteps = value;
    };

    loop.onChange = [this] (int value)
    {
        processor.getMarkovChain().states[(size_t) stateIndex].loopSteps = value;
    };

    refreshFromProcessor();
}

void ChainStateRow::resized()
{
    auto area = getLocalBounds().reduced (2, 1);
    area.removeFromLeft (24);

    const int gap = 4;
    const int width = juce::jmax (24, (area.getWidth() - 2 * gap) / 3);

    pattern.setBounds (area.removeFromLeft (width));
    area.removeFromLeft (gap);
    offset.setBounds (area.removeFromLeft (width));
    area.removeFromLeft (gap);
    loop.setBounds (area);
}

void ChainStateRow::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    g.setColour (active ? theme::accent.withAlpha (0.10f)
                        : (stateIndex % 2 == 0 ? theme::panelAlt.withAlpha (0.35f)
                                               : theme::panel.withAlpha (0.35f)));
    g.fillRoundedRectangle (bounds, 2.0f);

    if (active)
    {
        g.setColour (theme::accent);
        g.fillRoundedRectangle (bounds.removeFromLeft (2.0f), 1.0f);
    }

    g.setColour (active ? theme::accent : theme::textDim);
    g.setFont (theme::font (10.0f, active ? juce::Font::bold : juce::Font::plain));
    g.drawText (juce::String (stateIndex + 1),
                getLocalBounds().removeFromLeft (24),
                juce::Justification::centred, false);
}

void ChainStateRow::refreshFromProcessor()
{
    const auto& state = processor.getMarkovChain().states[(size_t) stateIndex];

    pattern.setValue (state.pattern + 1, juce::dontSendNotification);
    offset.setValue (state.offsetSteps, juce::dontSendNotification);
    loop.setValue (state.loopSteps, juce::dontSendNotification);
}

void ChainStateRow::setActive (bool shouldBeActive)
{
    if (active == shouldBeActive)
        return;

    active = shouldBeActive;
    repaint();
}

//==============================================================================
ChainEditorComponent::ChainEditorComponent (DrumKitProcessor& owner)
    : processor (owner), matrix (owner)
{
    chainOn.setTooltip ("Use the chain instead of the live SEQ pattern.");
    chainOn.onClick = [this]
    {
        processor.getMarkovChain().enabled = chainOn.getToggleState();
        updateStatus();
        updateActiveLabel();
    };
    addAndMakeVisible (chainOn);

    patternSelect.setTooltip ("Pattern slot to capture into or load from.");
    patternSelect.onChange = [this] (int) { updateStatus(); };
    patternSelect.setValue (1, juce::dontSendNotification);
    addAndMakeVisible (patternSelect);

    captureButton.setTooltip ("Copy the live SEQ/AUTO page into the selected pattern slot.");
    captureButton.onClick = [this]
    {
        processor.capturePattern (patternSelect.getValue() - 1);
        updateStatus();
        repaint();
    };
    addAndMakeVisible (captureButton);

    loadButton.setTooltip ("Copy the selected pattern into the live SEQ/AUTO page for editing.");
    loadButton.onClick = [this]
    {
        const int index = patternSelect.getValue() - 1;

        if (processor.getPattern (index).used)
        {
            processor.loadPattern (index);

            if (onEditRequested != nullptr)
                onEditRequested();
        }

        updateStatus();
    };
    addAndMakeVisible (loadButton);

    sequentialButton.setTooltip ("Reset every row to 100% to the next state; the last "
                                 "state goes back to the first.");
    sequentialButton.onClick = [this]
    {
        processor.getMarkovChain().resetToSequential();
        matrix.repaint();
    };
    addAndMakeVisible (sequentialButton);

    for (int i = 0; i < kNumChainStates; ++i)
    {
        rows[(size_t) i] = std::make_unique<ChainStateRow> (processor, i);
        addAndMakeVisible (*rows[(size_t) i]);
    }

    addAndMakeVisible (matrix);

    status.setFont (theme::font (10.0f));
    status.setColour (juce::Label::textColourId, theme::textDim);
    status.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (status);

    activeLabel.setFont (theme::font (10.0f, juce::Font::bold));
    activeLabel.setColour (juce::Label::textColourId, theme::accent);
    activeLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (activeLabel);

    syncControls();
}

void ChainEditorComponent::resized()
{
    auto area = getLocalBounds().reduced (12, 8);

    topArea = area.removeFromTop (30);
    {
        auto row = topArea;

        chainOn.setBounds (row.removeFromLeft (54));
        row.removeFromLeft (10);

        patternSelect.setBounds (row.removeFromLeft (120));
        row.removeFromLeft (6);

        captureButton.setBounds (row.removeFromLeft (66));
        row.removeFromLeft (4);

        loadButton.setBounds (row.removeFromLeft (52));
        row.removeFromLeft (8);

        sequentialButton.setBounds (row.removeFromLeft (82));
        row.removeFromLeft (10);

        activeLabel.setBounds (row.removeFromRight (juce::jmin (230, row.getWidth() / 2)));
        status.setBounds (row);
    }

    area.removeFromTop (6);

    const int tableWidth = juce::jlimit (300, 390, area.getWidth() / 3);
    tableArea = area.removeFromLeft (tableWidth);
    tableHeaderArea = tableArea.removeFromTop (18);

    const int rowHeight = juce::jlimit (20, 24, tableArea.getHeight() / kNumChainStates);

    for (int i = 0; i < kNumChainStates; ++i)
        rows[(size_t) i]->setBounds (tableArea.getX(),
                                     tableHeaderArea.getBottom() + i * rowHeight,
                                     tableArea.getWidth(),
                                     rowHeight);

    hintArea = tableArea.withTrimmedTop (tableHeaderArea.getHeight()
                                          + rowHeight * kNumChainStates + 8);

    area.removeFromLeft (12);
    matrixArea = area;
    matrix.setBounds (matrixArea);
}

void ChainEditorComponent::paint (juce::Graphics& g)
{
    const auto panel = getLocalBounds().toFloat().reduced (1.0f);

    g.setColour (theme::panel);
    g.fillRoundedRectangle (panel, 6.0f);
    g.setColour (theme::outline);
    g.drawRoundedRectangle (panel, 6.0f, 1.0f);

    // ---- State table header ---------------------------------------------------
    auto header = tableHeaderArea;

    g.setFont (theme::font (9.0f));
    g.setColour (theme::textDim);
    g.drawText ("STATE", header.removeFromLeft (24), juce::Justification::centred, false);

    const int gap = 4;
    const int width = (header.getWidth() - 2 * gap) / 3;

    g.drawText ("PATTERN", header.removeFromLeft (width), juce::Justification::centred, false);
    header.removeFromLeft (gap);
    g.drawText ("OFFSET /16", header.removeFromLeft (width), juce::Justification::centred, false);
    header.removeFromLeft (gap);
    g.drawText ("LOOP /16", header, juce::Justification::centred, false);

    g.setColour (theme::outline.withAlpha (0.7f));
    g.fillRect (tableHeaderArea.getX(), tableHeaderArea.getBottom(),
                tableHeaderArea.getWidth(), 1);

    // ---- Hint under the table -------------------------------------------------
    g.setColour (theme::textDim);
    g.setFont (theme::font (9.0f));
    g.drawFittedText ("Each row is the state being left; each knob is the chance of moving "
                      "to the column's state. Rows sum to 1.\n"
                      "Drag a knob vertically; right-click zeroes a cell.",
                      hintArea, juce::Justification::topLeft, 3);
}

void ChainEditorComponent::refreshFromProcessor()
{
    const uint32_t generation = processor.getUiGeneration();

    if (generation != lastGeneration)
    {
        lastGeneration = generation;
        syncControls();
    }

    const int active = processor.getChainActiveState();

    if (active != lastActiveState)
    {
        lastActiveState = active;
        matrix.setActiveState (active);

        for (int i = 0; i < kNumChainStates; ++i)
            rows[(size_t) i]->setActive (i == active);

        updateActiveLabel();
    }
}

void ChainEditorComponent::syncControls()
{
    chainOn.setToggleState (processor.getMarkovChain().enabled, juce::dontSendNotification);

    for (auto& row : rows)
        row->refreshFromProcessor();

    matrix.refreshFromProcessor();
    updateStatus();
    updateActiveLabel();

    lastGeneration = processor.getUiGeneration();
}

void ChainEditorComponent::updateStatus()
{
    const int index = juce::jlimit (0, kNumPatterns - 1, patternSelect.getValue() - 1);
    const bool used = processor.getPattern (index).used;

    status.setText (used ? "Pattern " + juce::String (index + 1) + " captured"
                         : "Pattern " + juce::String (index + 1)
                             + " empty - capture the SEQ page",
                   juce::dontSendNotification);

    status.setColour (juce::Label::textColourId, used ? theme::textDim : theme::velocityHard);
}

void ChainEditorComponent::updateActiveLabel()
{
    const int active = processor.getChainActiveState();

    if (active >= 0 && active < kNumChainStates)
    {
        const int pattern = juce::jlimit (0, kNumPatterns - 1,
                                          processor.getChainActivePattern());

        const bool used = processor.getPattern (pattern).used;

        activeLabel.setText ("State " + juce::String (active + 1)
                               + " - pattern " + juce::String (pattern + 1)
                               + (used ? "" : " (live)"),
                             juce::dontSendNotification);
    }
    else
    {
        activeLabel.setText (processor.getMarkovChain().enabled ? "Chain armed" : "Chain off",
                             juce::dontSendNotification);
    }
}

} // namespace df
