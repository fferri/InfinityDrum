#include "SequencerLane.h"

#include <algorithm>
#include <cmath>

#include "../Core/VelocityModulation.h"

namespace df
{

//==============================================================================
ValueStepper::ValueStepper (juce::String captionText, int minValue, int maxValue)
    : caption (std::move (captionText)), minimum (minValue), maximum (maxValue)
{
    for (auto* button : { &minus, &plus })
    {
        button->setConnectedEdges (juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
        addAndMakeVisible (*button);
    }

    minus.onClick = [this] { nudge (-1); };
    plus.onClick  = [this] { nudge (1); };
}

void ValueStepper::setValue (int newValue, juce::NotificationType notification)
{
    const int clamped = juce::jlimit (minimum, maximum, newValue);

    if (clamped == value)
        return;

    value = clamped;
    repaint();

    if (notification != juce::dontSendNotification && onChange != nullptr)
        onChange (value);
}

void ValueStepper::setLimits (int minValue, int maxValue)
{
    minimum = minValue;
    maximum = maxValue;
    value = juce::jlimit (minimum, maximum, value);
    repaint();
}

void ValueStepper::nudge (int delta)
{
    setValue (value + delta, juce::sendNotificationSync);
}

void ValueStepper::resized()
{
    auto area = getLocalBounds();

    plus.setBounds (area.removeFromRight (18));
    minus.setBounds (area.removeFromRight (18));
}

void ValueStepper::paint (juce::Graphics& g)
{
    auto area = getLocalBounds();

    g.setColour (theme::panelAlt);
    g.fillRoundedRectangle (area.toFloat(), 3.0f);

    g.setColour (theme::textDim);
    g.setFont (theme::font (9.0f));
    g.drawText (caption, area.removeFromLeft (38).reduced (5, 0),
                juce::Justification::centredLeft, false);

    // The two buttons are children drawn on top, so their width has to come out of the area
    // the number is centred in. Otherwise a three-digit count runs underneath them.
    area.removeFromRight (2 * 18);

    g.setColour (theme::text);
    g.setFont (theme::font (11.0f, juce::Font::bold));
    g.drawText (juce::String (value), area, juce::Justification::centred, false);
}

//==============================================================================
SequencerLaneComponent::SequencerLaneComponent (DrumKitProcessor& owner, int index)
    : processor (owner), trackIndex (index), header (owner, index)
{
    // Every Rng default-constructs to the same seed, so without this each track's die rolled
    // an identical pattern - the same "random" lane repeated eight times. The multiplier is
    // Knuth's, which scatters adjacent indices well enough for eight of them.
    // Seeded per track, or every row's die would roll the same pattern.
    randomRng.seed (randomSeedForTrack (index));

    addAndMakeVisible (header);
    modeBox.setTooltip ("How the pattern is built: an explicit grid, or pulses spread "
                        "evenly across the steps.");
    speedBox.setTooltip ("How fast. A step is a 1/16 note divided by this, so a bigger "
                         "number is faster. Off stops the lane.");
    lengthStepper.setBoundsTo (getLocalBounds());
    lengthStepper.setTooltip ("How many steps before the pattern repeats. Independent of "
                              "speed, which is what allows polymeters.");
    trigsStepper.setTooltip ("How many pulses to spread across the steps.");
    offsetStepper.setTooltip ("Where the pattern starts. Negative walks it backwards.");

    modeBox.addListener (this);
    speedBox.addListener (this);
    addAndMakeVisible (modeBox);
    addAndMakeVisible (speedBox);

    enabledButton.setTooltip ("Whether this lane plays.");
    enabledButton.onClick = [this] { lane().enabled = enabledButton.getToggleState(); repaint(); };
    addAndMakeVisible (enabledButton);

    randomiseButton.setTooltip ("Roll a new speed and pattern, and switch the lane on.\n"
                                "The length is kept, so the cycle you set stays put.");
    randomiseButton.onClick = [this]
    {
        randomiseTriggerLane (lane(), randomRng);
        syncControls();
        repaint();
    };
    addAndMakeVisible (randomiseButton);

    for (int i = 0; i < kNumTriggerModes; ++i)
        modeBox.addItem (triggerModeName ((TriggerMode) i), i + 1);

    for (int i = 0; i < kNumLaneSpeeds; ++i)
        speedBox.addItem (laneSpeedName ((LaneSpeed) i), i + 1);

    lengthStepper.onChange = [this] (int v) { lane().length = v; clampEuclidean(); repaint(); };
    trigsStepper.onChange  = [this] (int v) { lane().trigs = v; repaint(); };
    offsetStepper.onChange = [this] (int v) { lane().offset = v; repaint(); };

    addAndMakeVisible (lengthStepper);
    addAndMakeVisible (trigsStepper);
    addAndMakeVisible (offsetStepper);

    syncControls();
}

void SequencerLaneComponent::clampEuclidean()
{
    auto& current = lane();
    current.trigs = juce::jlimit (0, current.length, current.trigs);
    current.offset = juce::jlimit (-current.length, current.length, current.offset);
}

TriggerLane& SequencerLaneComponent::lane() noexcept
{
    return processor.getSequencerState().triggers[(size_t) trackIndex];
}

const TriggerLane& SequencerLaneComponent::lane() const noexcept
{
    return processor.getSequencerState().triggers[(size_t) trackIndex];
}

void SequencerLaneComponent::refreshFromProcessor()
{
    header.updateFromProcessor();
    syncControls();
    repaint();
}

void SequencerLaneComponent::syncControls()
{
    const auto& current = lane();

    enabledButton.setToggleState (current.enabled, juce::dontSendNotification);
    modeBox.setSelectedId ((int) current.mode + 1, juce::dontSendNotification);
    speedBox.setSelectedId ((int) current.speed + 1, juce::dontSendNotification);

    lengthStepper.setValue (current.length, juce::dontSendNotification);
    trigsStepper.setLimits (0, current.length);
    trigsStepper.setValue (current.trigs, juce::dontSendNotification);
    offsetStepper.setLimits (-current.length, current.length);
    offsetStepper.setValue (current.offset, juce::dontSendNotification);

    const bool euclidean = current.mode == TriggerMode::euclidean;

    trigsStepper.setVisible (euclidean);
    offsetStepper.setVisible (euclidean);
}

void SequencerLaneComponent::comboBoxChanged (juce::ComboBox* box)
{
    auto& current = lane();

    if (box == &modeBox)
    {
        current.setDefaultsFor ((TriggerMode) juce::jmax (0, modeBox.getSelectedId() - 1));
        clampEuclidean();
    }
    else if (box == &speedBox)
    {
        current.speed = (LaneSpeed) juce::jlimit (0, kNumLaneSpeeds - 1, speedBox.getSelectedId() - 1);
    }

    syncControls();
    resized();
    repaint();
}

//==============================================================================
void SequencerLaneComponent::resized()
{
    auto area = getLocalBounds().reduced (8, 5);

    // The same header every page uses, so a track's identity never moves.
    headerArea = area.removeFromLeft (TrackHeader::identityWidth
                                        + juce::jmin (area.getHeight(), area.getWidth() / 5));
    header.setBounds (headerArea);
    area.removeFromLeft (8);

    controlsArea = area.removeFromLeft (118);
    {
        auto stack = controlsArea;
        const int cellHeight = juce::jmin (19, (stack.getHeight() - 6) / 4);

        modeBox.setBounds (stack.removeFromTop (cellHeight));
        stack.removeFromTop (2);
        speedBox.setBounds (stack.removeFromTop (cellHeight));
        stack.removeFromTop (2);
        lengthStepper.setBounds (stack.removeFromTop (cellHeight));
        stack.removeFromTop (2);

        auto lastRow = stack.removeFromTop (cellHeight);
        enabledButton.setBounds (lastRow.removeFromLeft (76));
        randomiseButton.setBounds (lastRow.removeFromLeft (32).reduced (2, 0));
    }

    area.removeFromLeft (10);

    euclideanArea = area.removeFromRight (150);
    {
        auto stack = euclideanArea.withSizeKeepingCentre (150, juce::jmin (euclideanArea.getHeight(), 44));

        trigsStepper.setBounds (stack.removeFromTop (20));
        stack.removeFromTop (2);
        offsetStepper.setBounds (stack.removeFromTop (20));
    }

    editorBounds = area.withTrimmedRight (10);
}

int SequencerLaneComponent::stepIndexAt (juce::Point<int> position) const noexcept
{
    if (editorBounds.isEmpty())
        return -1;

    const int columns = juce::jmax (1, lane().length);
    const float relative = (float) (position.x - editorBounds.getX())
                             / (float) juce::jmax (1, editorBounds.getWidth());

    return juce::jlimit (0, columns - 1, (int) std::floor (relative * (float) columns));
}

float SequencerLaneComponent::velocityFromY (int y) const noexcept
{
    if (editorBounds.isEmpty())
        return 100.0f;

    const float relative = 1.0f - (float) (y - editorBounds.getY())
                                    / (float) juce::jmax (1, editorBounds.getHeight());

    return juce::jlimit (1.0f, 127.0f, std::round (relative * 127.0f));
}

void SequencerLaneComponent::mouseDown (const juce::MouseEvent& event)
{
    auto& current = lane();

    if (current.mode != TriggerMode::grid || ! editorBounds.contains (event.getPosition()))
        return;

    const int step = stepIndexAt (event.getPosition());

    if (! juce::isPositiveAndBelow (step, current.length))
        return;

    // A click toggles; a vertical drag sets velocity. Which one it was is only known on
    // release, so nothing is committed yet.
    activeStep = step;
    dragStartY = event.y;
    dragged = false;
}

void SequencerLaneComponent::mouseDrag (const juce::MouseEvent& event)
{
    auto& current = lane();

    if (activeStep < 0 || activeStep >= current.length || current.mode != TriggerMode::grid)
        return;

    if (! dragged && std::abs (event.y - dragStartY) < 3)
        return;      // still a click, not yet a drag

    dragged = true;

    // Dragging a step up or down turns it on: drawing a velocity into an empty step is how
    // you program a pattern, and an off step has no velocity worth setting.
    current.steps[(size_t) activeStep].on = true;
    current.steps[(size_t) activeStep].velocity = velocityFromY (event.y);

    repaint (editorBounds);
}

void SequencerLaneComponent::mouseUp (const juce::MouseEvent&)
{
    auto& current = lane();

    if (activeStep >= 0 && activeStep < current.length && ! dragged
         && current.mode == TriggerMode::grid)
    {
        auto& step = current.steps[(size_t) activeStep];
        step.on = ! step.on;

        // Re-enabling a step without touching its velocity should give it the default
        // rather than whatever it was left at before it was switched off.
        if (step.on && step.velocity < 1.0f)
            step.velocity = 100.0f;

        repaint (editorBounds);
    }

    activeStep = -1;
    dragged = false;
}

void SequencerLaneComponent::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (1.0f);

    g.setColour (theme::panel);
    g.fillRoundedRectangle (bounds, 5.0f);
    g.setColour (theme::outline);
    g.drawRoundedRectangle (bounds, 5.0f, 1.0f);

    if (editorBounds.isEmpty())
        return;

    const auto& current = lane();
    const auto grid = editorBounds.toFloat();

    g.setColour (juce::Colour (0xff101317));
    g.fillRoundedRectangle (grid, 3.0f);

    const bool running = current.enabled;
    const int columns = juce::jmax (1, current.length);
    const float cellWidth = grid.getWidth() / (float) columns;
    const float inset = juce::jmin (2.0f, cellWidth * 0.15f);

    for (int step = 0; step < columns; ++step)
    {
        const auto cell = juce::Rectangle<float> (grid.getX() + cellWidth * (float) step,
                                                  grid.getY(),
                                                  cellWidth,
                                                  grid.getHeight()).reduced (inset);

        // The cell's background is only a beat marker; the state is drawn inside it.
        g.setColour (step % 4 == 0 ? theme::panelAlt.brighter (0.22f) : theme::panelAlt);
        g.fillRoundedRectangle (cell, 2.0f);

        if (current.mode == TriggerMode::euclidean)
        {
            if (running && current.isStepOn (step))
            {
                g.setColour (theme::accent);
                g.fillRoundedRectangle (cell.reduced (0.5f), 2.0f);
            }

            continue;
        }

        const auto& gridStep = current.steps[(size_t) step];
        const bool on = running && gridStep.on;

        // Velocity as a bar growing upwards from the bottom, so a glance down the row is a
        // picture of the dynamics.
        if (on)
        {
            const float amount = juce::jlimit (0.0f, 1.0f, gridStep.velocity / 127.0f);
            const float barHeight = juce::jmax (1.5f, (cell.getHeight() - 2.0f) * amount);

            auto bar = cell.reduced (1.0f);
            bar = bar.withTrimmedTop (bar.getHeight() - barHeight);

            g.setColour (theme::accent.withAlpha (0.9f));
            g.fillRect (bar);
        }

        // A separate outline, so "this step fires" is legible even when its velocity is low
        // and the bar is only a sliver.
        g.setColour (on ? theme::accent : theme::outline);
        g.drawRoundedRectangle (cell.reduced (0.5f), 2.0f, on ? 1.6f : 1.0f);
    }

    // ---- Playhead ------------------------------------------------------------
    if (running)
    {
        const float x = grid.getX()
                          + processor.getTriggerPhase (trackIndex) * grid.getWidth();

        g.setColour (theme::text.withAlpha (0.75f));
        g.fillRect (juce::Rectangle<float> (x - 1.0f, grid.getY(), 2.0f, grid.getHeight()));
    }
}

//==============================================================================
AutomationLaneComponent::AutomationLaneComponent (DrumKitProcessor& owner, int index)
    : processor (owner), trackIndex (index), header (owner, index)
{
    randomRng.seed ((uint32_t) (index + 1) * 2654435761u);

    addAndMakeVisible (header);
    speedBox.setTooltip ("How fast the envelope runs.");
    lengthStepper.setBoundsTo (getLocalBounds());
    lengthStepper.setTooltip ("How many steps the envelope spans before it loops. Its own "
                              "length, independent of the track's trigger sequencer.");

    speedBox.addListener (this);
    addAndMakeVisible (speedBox);

    enabledButton.setTooltip ("Whether this automation lane runs.");
    enabledButton.onClick = [this] { lane().enabled = enabledButton.getToggleState(); repaint(); };
    addAndMakeVisible (enabledButton);

    randomiseButton.setTooltip ("Draw a new envelope for this parameter, and switch the lane "
                                "on.\nSpeed and length are kept.");
    randomiseButton.onClick = [this]
    {
        randomiseAutomationLane (lane(), randomRng);
        syncControls();
        repaint();
    };
    addAndMakeVisible (randomiseButton);


    for (int i = 0; i < kNumLaneSpeeds; ++i)
        speedBox.addItem (laneSpeedName ((LaneSpeed) i), i + 1);

    amountSlider.setSliderStyle (juce::Slider::LinearBar);
    amountSlider.setTextBoxStyle (juce::Slider::TextBoxAbove, false, 56, 13);
    amountSlider.setRange (0.0, 100.0, 1.0);
    amountSlider.setTooltip ("How far the envelope moves the parameter.");
    amountSlider.addListener (this);
    addAndMakeVisible (amountSlider);

    lengthStepper.onChange = [this] (int v) { lane().length = v; repaint(); };
    addAndMakeVisible (lengthStepper);

    syncControls();
}

AutomationLane& AutomationLaneComponent::lane() noexcept
{
    return processor.getSequencerState()
        .automation[(size_t) trackIndex][(size_t) juce::jlimit (0, kMaxAutomationLanes - 1, slot)];
}

const AutomationLane& AutomationLaneComponent::lane() const noexcept
{
    return laneAt (slot);
}

const AutomationLane& AutomationLaneComponent::laneAt (int index) const noexcept
{
    return processor.getSequencerState()
        .automation[(size_t) trackIndex][(size_t) juce::jlimit (0, kMaxAutomationLanes - 1, index)];
}

juce::Rectangle<int> AutomationLaneComponent::tabBounds (int index) const noexcept
{
    if (tabArea.isEmpty() || ! juce::isPositiveAndBelow (index, kMaxAutomationLanes))
        return {};

    const int width = tabArea.getWidth() / kMaxAutomationLanes;

    return juce::Rectangle<int> (tabArea.getX() + width * index, tabArea.getY(),
                                 width, tabArea.getHeight()).reduced (1, 0);
}

int AutomationLaneComponent::tabIndexAt (juce::Point<int> position) const noexcept
{
    for (int i = 0; i < kMaxAutomationLanes; ++i)
        if (tabBounds (i).contains (position))
            return i;

    return -1;
}

void AutomationLaneComponent::refreshFromProcessor()
{
    header.updateFromProcessor();
    syncControls();
    repaint();
}

void AutomationLaneComponent::syncControls()
{
    const auto& current = lane();

    enabledButton.setToggleState (current.enabled, juce::dontSendNotification);
    speedBox.setSelectedId ((int) current.speed + 1, juce::dontSendNotification);
    lengthStepper.setValue (current.length, juce::dontSendNotification);
    amountSlider.setValue ((double) current.amount * 100.0f, juce::dontSendNotification);
}

void AutomationLaneComponent::comboBoxChanged (juce::ComboBox* box)
{
    auto& current = lane();

    if (box == &speedBox)
        current.speed = (LaneSpeed) juce::jlimit (0, kNumLaneSpeeds - 1, speedBox.getSelectedId() - 1);

    repaint();
}

void AutomationLaneComponent::sliderValueChanged (juce::Slider*)
{
    lane().amount = (float) amountSlider.getValue() / 100.0f;
    repaint();
}

void AutomationLaneComponent::resized()
{
    auto area = getLocalBounds().reduced (8, 4);

    // Reserve the strip's height here; its width is set at the end, once the envelope's
    // column is known.
    const int rowTop = area.getY();
    area.removeFromTop (tabHeight + 2);

    headerArea = area.removeFromLeft (TrackHeader::identityWidth
                                        + juce::jmin (area.getHeight(), area.getWidth() / 5));
    header.setBounds (headerArea);
    area.removeFromLeft (8);

    controlsArea = area.removeFromLeft (118);
    {
        auto stack = controlsArea;
        const int cellHeight = juce::jmin (19, (stack.getHeight() - 4) / 3);

        speedBox.setBounds (stack.removeFromTop (cellHeight));
        stack.removeFromTop (2);
        lengthStepper.setBounds (stack.removeFromTop (cellHeight));
        stack.removeFromTop (2);

        auto lastRow = stack.removeFromTop (cellHeight);
        enabledButton.setBounds (lastRow.removeFromLeft (76));
        randomiseButton.setBounds (lastRow.removeFromLeft (32).reduced (2, 0));
    }

    area.removeFromLeft (10);

    // The slider's own readout says 50, which means nothing without a caption; the caption
    // sits in the strip above it.
    auto amountArea = area.removeFromRight (110);
    amountLabelArea = amountArea.removeFromTop (13).withTrimmedLeft (2);
    amountSlider.setBounds (amountArea.removeFromTop (juce::jmin (32, amountArea.getHeight())));

    editorBounds = area.withTrimmedRight (10);

    // The tabs line up with the envelope, not with the whole row, so the two read as one
    // column instead of a bar that happens to be above it.
    tabArea = juce::Rectangle<int> (editorBounds.getX(), rowTop,
                                    editorBounds.getWidth(), tabHeight);
}

juce::Rectangle<int> AutomationLaneComponent::envelopeBounds() const noexcept
{
    // Inset, so a point at position 0 or 1 or at value -1 or +1 is drawn wholly inside the
    // component. They used to sit centred on the boundary, and Component::contains() excludes
    // the right and bottom edges, which made the first and last points impossible to grab.
    return editorBounds.reduced (5);
}

int AutomationLaneComponent::findPointAt (juce::Point<int> position) const noexcept
{
    const auto& current = lane();
    const auto area = envelopeBounds();

    if (current.numPoints <= 0 || area.isEmpty())
        return -1;

    int best = -1;
    int bestDistance = 14;

    for (int i = 0; i < current.numPoints; ++i)
    {
        const auto& point = current.points[(size_t) i];
        const int x = area.getX() + juce::roundToInt (point.position * (float) area.getWidth());
        const int y = area.getCentreY()
                        - juce::roundToInt (point.value * (float) area.getHeight() * 0.5f);

        const int distance = juce::jmax (std::abs (x - position.x), std::abs (y - position.y));

        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = i;
        }
    }

    return best;
}

void AutomationLaneComponent::mouseDown (const juce::MouseEvent& event)
{
    if (const int tab = tabIndexAt (event.getPosition()); tab >= 0)
    {
        slot = tab;
        syncControls();
        repaint();
        return;
    }

    auto& current = lane();

    const auto area = envelopeBounds();

    if (! editorBounds.contains (event.getPosition()))
        return;

    draggedPoint = findPointAt (event.getPosition());

    if (draggedPoint < 0 && current.numPoints < kMaxModPoints)
    {
        const float position = juce::jlimit (0.0f, 1.0f,
            (float) (event.x - area.getX()) / (float) juce::jmax (1, area.getWidth()));
        const float value = juce::jlimit (-1.0f, 1.0f,
            -((float) (event.y - area.getCentreY())
                / (float) juce::jmax (1, area.getHeight()) * 2.0f));

        int insertAt = current.numPoints;

        for (int i = 0; i < current.numPoints; ++i)
        {
            if (current.points[(size_t) i].position > position)
            {
                insertAt = i;
                break;
            }
        }

        for (int i = current.numPoints; i > insertAt; --i)
            current.points[(size_t) i] = current.points[(size_t) (i - 1)];

        current.points[(size_t) insertAt] = { position, value };
        ++current.numPoints;
        draggedPoint = insertAt;
    }

    repaint (editorBounds);
}

void AutomationLaneComponent::mouseDrag (const juce::MouseEvent& event)
{
    auto& current = lane();

    if (draggedPoint < 0 || draggedPoint >= current.numPoints)
        return;

    // Clamp between the neighbours so the points stay ordered; re-sorting mid-drag would
    // swap the point out from under the pointer.
    const auto area = envelopeBounds();
    const float lower = draggedPoint > 0 ? current.points[(size_t) (draggedPoint - 1)].position : 0.0f;
    const float upper = draggedPoint < current.numPoints - 1
                          ? current.points[(size_t) (draggedPoint + 1)].position
                          : 1.0f;

    current.points[(size_t) draggedPoint].position = juce::jlimit (lower, upper,
        (float) (event.x - area.getX()) / (float) juce::jmax (1, area.getWidth()));

    current.points[(size_t) draggedPoint].value = juce::jlimit (-1.0f, 1.0f,
        -((float) (event.y - area.getCentreY())
            / (float) juce::jmax (1, area.getHeight()) * 2.0f));

    // The whole component, not just the envelope: a dot at the edge used to leave a stale
    // half drawn outside the repainted region.
    repaint();
}

void AutomationLaneComponent::mouseUp (const juce::MouseEvent&)
{
    draggedPoint = -1;
}

void AutomationLaneComponent::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (1.0f);

    g.setColour (theme::panel);
    g.fillRoundedRectangle (bounds, 5.0f);
    g.setColour (theme::outline);
    g.drawRoundedRectangle (bounds, 5.0f, 1.0f);

    // ---- Parameter tabs ------------------------------------------------------
    // One per controllable parameter. The base is neutral, the current one is outlined, and
    // a filled one means that parameter has automation running - so the row says at a glance
    // what is being automated, not just what you are looking at.
    for (int i = 0; i < kMaxAutomationLanes; ++i)
    {
        const auto tab = tabBounds (i);

        if (tab.isEmpty())
            continue;

        const bool selected = i == slot;
        const bool active = laneAt (i).enabled;

        g.setColour (active ? (selected ? theme::accent : theme::accent.withAlpha (0.5f))
                            : theme::panelAlt);
        g.fillRoundedRectangle (tab.toFloat(), 2.0f);

        if (selected)
        {
            g.setColour (theme::text);
            g.drawRoundedRectangle (tab.toFloat().reduced (0.5f), 2.0f, 1.4f);
        }

        g.setColour (active ? (selected ? theme::background : theme::text) : theme::textDim);
        g.setFont (theme::font (9.0f, selected ? juce::Font::bold : juce::Font::plain));
        g.drawText (modTargetName (i), tab, juce::Justification::centred, false);
    }

    if (editorBounds.isEmpty())
        return;

    g.setColour (theme::textDim);
    g.setFont (theme::font (9.0f));
    g.drawText ("Amount", amountLabelArea, juce::Justification::centredLeft, false);

    const auto& current = lane();

    g.setColour (juce::Colour (0xff101317));
    g.fillRoundedRectangle (editorBounds.toFloat(), 3.0f);

    if (! current.enabled)
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (10.0f));
        g.drawText ("off", editorBounds, juce::Justification::centred, false);
        return;
    }

    const auto area = envelopeBounds().toFloat();

    g.setColour (theme::outline);
    g.fillRect (area.getX(), area.getCentreY() - 0.5f, area.getWidth(), 1.0f);

    juce::Path path;
    bool started = false;

    for (int i = 0; i < current.numPoints; ++i)
    {
        const auto& point = current.points[(size_t) i];
        const float x = area.getX() + point.position * area.getWidth();
        const float y = area.getCentreY() - point.value * area.getHeight() * 0.5f;

        if (! started)
        {
            path.startNewSubPath (x, y);
            started = true;
        }
        else
        {
            path.lineTo (x, y);
        }
    }

    if (started)
    {
        g.setColour (theme::accent.withAlpha (0.85f));
        g.strokePath (path, juce::PathStrokeType (1.6f));
    }

    for (int i = 0; i < current.numPoints; ++i)
    {
        const auto& point = current.points[(size_t) i];
        const float x = area.getX() + point.position * area.getWidth();
        const float y = area.getCentreY() - point.value * area.getHeight() * 0.5f;

        g.setColour (i == draggedPoint ? theme::accent : theme::text);
        g.fillEllipse (x - 3.5f, y - 3.5f, 7.0f, 7.0f);
    }

    // ---- Playhead ------------------------------------------------------------
    {
        const float x = area.getX()
                          + processor.getAutomationPhase (trackIndex, slot) * area.getWidth();

        g.setColour (theme::text.withAlpha (0.7f));
        g.fillRect (juce::Rectangle<float> (x - 1.0f, area.getY(), 2.0f, area.getHeight()));
    }
}

} // namespace df
