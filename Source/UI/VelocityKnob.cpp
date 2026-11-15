#include "VelocityKnob.h"

namespace df
{

namespace
{
    /** Pixels of vertical drag for a full -1..+1 sweep of the depth. */
    constexpr float depthDragPixels = 180.0f;
}

//==============================================================================
juce::Point<float> VelocityKnob::Geometry::pointAt (float normalised) const noexcept
{
    const float angle = angleFor (normalised);

    // Matches the convention JUCE uses for the rotary thumb, so the dot sits on the arc
    // wherever the pointer does.
    return { centre.x + arcRadius * std::sin (angle),
             centre.y - arcRadius * std::cos (angle) };
}

VelocityKnob::VelocityKnob (DrumKitProcessor& owner,
                            int index,
                            int target,
                            EditorUiState& state)
    : processor (owner),
      trackIndex (index),
      modTarget (target),
      uiState (state)
{
    setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    setVelocityBasedMode (false);
    setColour (juce::Slider::rotarySliderFillColourId, theme::accent);
    setColour (juce::Slider::rotarySliderOutlineColourId, theme::outline);

    displayedVelocity = processor.getLastVelocity (trackIndex);
}

void VelocityKnob::setAccentColour (juce::Colour newAccent)
{
    accent = newAccent;
    setColour (juce::Slider::rotarySliderFillColourId, newAccent);
    repaint();
}

float VelocityKnob::getDepth() const
{
    return processor.getModulationDepth (trackIndex, modTarget);
}

VelocityKnob::Geometry VelocityKnob::getGeometry() const
{
    const auto bounds = getLocalBounds().toFloat().reduced (1.5f);
    const auto radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto params = getRotaryParameters();

    Geometry geometry;
    geometry.centre = bounds.getCentre();
    geometry.radius = radius;
    geometry.lineWidth = juce::jmax (2.0f, radius * 0.11f);
    geometry.arcRadius = juce::jmax (1.0f, radius - geometry.lineWidth * 0.5f);
    geometry.bodyRadius = juce::jmax (2.0f, geometry.arcRadius - geometry.lineWidth * 0.85f);
    geometry.bandRadius = radius * 0.78f;
    geometry.bandWidth = juce::jmax (2.0f, radius * 0.13f);
    geometry.startAngle = params.startAngleRadians;
    geometry.endAngle = params.endAngleRadians;

    return geometry;
}

float VelocityKnob::getCentrePosition() const
{
    return juce::jlimit (0.0f, 1.0f,
                         (float) getNormalisableRange().convertTo0to1 (getValue()));
}

bool VelocityKnob::isInRim (juce::Point<float> position) const
{
    const auto geometry = getGeometry();
    return geometry.centre.getDistanceFrom (position) > geometry.bodyRadius;
}

bool VelocityKnob::shouldEditDepth (const juce::MouseEvent& event) const
{
    return isModulatable() && (uiState.isModulationMode() || isInRim (event.position));
}

//==============================================================================
void VelocityKnob::paint (juce::Graphics& g)
{
    const auto geometry = getGeometry();

    if (geometry.arcRadius < 3.0f)
        return;

    const auto fillColour = findColour (juce::Slider::rotarySliderFillColourId);
    const auto trackColour = findColour (juce::Slider::rotarySliderOutlineColourId);
    const bool armed = uiState.isModulationMode();

    const float value = getCentrePosition();
    const float depth = getDepth();
    const bool modulated = std::abs (depth) > 1.0e-6f;

    const auto pointAt = [&] (float r, float normalised)
    {
        const float angle = geometry.angleFor (normalised);
        return juce::Point<float> (geometry.centre.x + r * std::sin (angle),
                                   geometry.centre.y - r * std::cos (angle));
    };

    const auto arcPath = [&] (float from, float to)
    {
        juce::Path path;
        path.addCentredArc (geometry.centre.x, geometry.centre.y, geometry.arcRadius,
                            geometry.arcRadius, 0.0f, from, to, true);
        return path;
    };

    const auto pathAt = [&] (float r, float from, float to)
    {
        juce::Path path;
        path.addCentredArc (geometry.centre.x, geometry.centre.y, r, r, 0.0f, from, to, true);
        return path;
    };

    const auto strokeGlow = [&] (const juce::Path& path, juce::Colour colour,
                                 float width, float spread, int layers)
    {
        for (int i = layers; i >= 1; --i)
        {
            const float k = (float) i / (float) layers;
            g.setColour (colour.withMultipliedAlpha (0.26f * (1.0f - 0.45f * k)));
            g.strokePath (path, juce::PathStrokeType (width + spread * 2.0f * k,
                                                      juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));
        }

        g.setColour (colour);
        g.strokePath (path, juce::PathStrokeType (width, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));
    };

    // ---- body ---------------------------------------------------------------
    {
        const auto body = juce::Rectangle<float> (geometry.bodyRadius * 2.0f,
                                                  geometry.bodyRadius * 2.0f)
                              .withCentre (geometry.centre);

        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff252b34), geometry.centre.x, body.getY(),
                                                 juce::Colour (0xff0a0d11), geometry.centre.x, body.getBottom(),
                                                 false));
        g.fillEllipse (body);

        g.setColour (juce::Colours::white.withAlpha (0.055f));
        g.drawEllipse (body.reduced (geometry.bodyRadius * 0.16f), 1.0f);
    }

    // ---- velocity band, on an inner orbit ------------------------------------
    if (modulated)
    {
        const float softEnd = juce::jlimit (0.0f, 1.0f, value - depth);
        const float hardEnd = juce::jlimit (0.0f, 1.0f, value + depth);
        const float low = juce::jmin (softEnd, hardEnd);
        const float high = juce::jmax (softEnd, hardEnd);

        if (high - low > 1.0e-4f)
        {
            // A dark bed keeps the band reading the same over the body edge and the panel.
            g.setColour (juce::Colour (0xff0a0c10));
            g.strokePath (pathAt (geometry.bandRadius, geometry.angleFor (low),
                                  geometry.angleFor (high)),
                          juce::PathStrokeType (geometry.bandWidth, juce::PathStrokeType::curved,
                                                juce::PathStrokeType::butt));

            constexpr int segments = 28;
            const float step = (high - low) / (float) segments;

            for (int i = 0; i < segments; ++i)
            {
                // Overlapping segments, so antialiased seams do not read as blocks.
                const float t0 = juce::jlimit (0.0f, 1.0f, low + step * ((float) i - 0.5f));
                const float t1 = juce::jlimit (0.0f, 1.0f, low + step * ((float) i + 1.5f));
                const float middle = 0.5f * (t0 + t1);
                const float velocity = juce::jlimit (0.0f, 1.0f,
                                                     0.5f + 0.5f * (middle - value) / depth);

                g.setColour (theme::velocitySoft.interpolatedWith (theme::velocityHard, velocity)
                                 .withAlpha (0.45f));
                g.strokePath (pathAt (geometry.bandRadius, geometry.angleFor (t0),
                                      geometry.angleFor (t1)),
                              juce::PathStrokeType (geometry.bandWidth, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::butt));
            }
        }
    }

    // ---- full sweep track ----------------------------------------------------
    // Half the value arc's weight: a groove the bright arc sits over, not a second arc.
    g.setColour (trackColour.withAlpha (0.85f));
    g.strokePath (arcPath (geometry.startAngle, geometry.endAngle),
                  juce::PathStrokeType (juce::jmax (1.0f, geometry.lineWidth * 0.5f),
                                        juce::PathStrokeType::curved,
                                        juce::PathStrokeType::rounded));

    // ---- pointer, under the ring so the two meet without a blob --------------
    {
        const float pointerWidth = juce::jmax (2.0f, geometry.radius * 0.08f);

        // The rounded cap's outer edge lands on the ring's inner edge, so the line
        // meets the arc instead of bulging through it.
        const float tipRadius = geometry.arcRadius
                                  - (pointerWidth * 0.5f + geometry.lineWidth * 0.5f);

        juce::Path pointer;
        pointer.startNewSubPath (pointAt (geometry.radius * 0.34f, value));
        pointer.lineTo (pointAt (tipRadius, value));

        if (hovering)
            strokeGlow (pointer, fillColour, pointerWidth, geometry.radius * 0.05f, 4);

        g.setColour (armed ? fillColour.withAlpha (0.55f) : fillColour.brighter (0.35f));
        g.strokePath (pointer, juce::PathStrokeType (pointerWidth, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
    }

    // ---- value arc -----------------------------------------------------------
    if (value > 0.002f)
    {
        const auto valuePath = arcPath (geometry.startAngle, geometry.angleFor (value));

        if (hovering)
        {
            strokeGlow (valuePath, fillColour, geometry.lineWidth, geometry.radius * 0.06f, 6);
        }
        else
        {
            g.setColour (armed ? fillColour.withAlpha (0.35f) : fillColour);
            g.strokePath (valuePath, juce::PathStrokeType (geometry.lineWidth,
                                                           juce::PathStrokeType::curved,
                                                           juce::PathStrokeType::rounded));
        }
    }

    // ---- drag clue: the band's ends, on hover only ---------------------------
    if (modulated && hovering)
    {
        const float softEnd = juce::jlimit (0.0f, 1.0f, value - depth);
        const float hardEnd = juce::jlimit (0.0f, 1.0f, value + depth);

        for (int end = 0; end < 2; ++end)
        {
            const float position = end == 0 ? softEnd : hardEnd;
            const auto colour = end == 0 ? theme::velocitySoft : theme::velocityHard;
            const auto point = pointAt (geometry.bandRadius, position);
            const auto size = juce::jmax (2.0f, geometry.radius * 0.10f);

            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.fillEllipse (juce::Rectangle<float> (size * 1.7f, size * 1.7f).withCentre (point));
            g.setColour (colour.brighter (0.3f));
            g.fillEllipse (juce::Rectangle<float> (size, size).withCentre (point));
        }
    }

    // ---- dot at where the last hit actually landed ---------------------------
    if (modulated)
    {
        const float bipolar = velocityBipolar (processor.getLastVelocity (trackIndex));
        const float effective = juce::jlimit (0.0f, 1.0f, value + depth * bipolar);
        const auto point = pointAt (geometry.arcRadius, effective);

        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.fillEllipse (juce::Rectangle<float> (geometry.lineWidth * 2.0f,
                                               geometry.lineWidth * 2.0f).withCentre (point));
        g.setColour (juce::Colours::white.withAlpha (0.95f));
        g.fillEllipse (juce::Rectangle<float> (geometry.lineWidth * 1.25f,
                                               geometry.lineWidth * 1.25f).withCentre (point));
    }
}

//==============================================================================
void VelocityKnob::mouseDown (const juce::MouseEvent& event)
{
    if (isModulatable() && event.mods.isPopupMenu())
    {
        showContextMenu();
        return;
    }

    if (shouldEditDepth (event))
    {
        draggingDepth = true;
        dragStartDepth = getDepth();
        return;   // deliberately not forwarded: the value must not move as well
    }

    juce::Slider::mouseDown (event);
}

void VelocityKnob::mouseDrag (const juce::MouseEvent& event)
{
    if (draggingDepth)
    {
        setDepth (dragStartDepth - (float) event.getDistanceFromDragStartY() / depthDragPixels);
        return;
    }

    juce::Slider::mouseDrag (event);
}

void VelocityKnob::mouseUp (const juce::MouseEvent& event)
{
    if (draggingDepth)
    {
        draggingDepth = false;
        repaint();
        return;
    }

    juce::Slider::mouseUp (event);
}

void VelocityKnob::mouseDoubleClick (const juce::MouseEvent& event)
{
    if (shouldEditDepth (event))
    {
        setDepth (0.0f);
        return;
    }

    juce::Slider::mouseDoubleClick (event);
}

void VelocityKnob::mouseEnter (const juce::MouseEvent& event)
{
    hovering = true;
    repaint();
    juce::Slider::mouseEnter (event);
}

void VelocityKnob::mouseExit (const juce::MouseEvent& event)
{
    hovering = false;
    repaint();
    juce::Slider::mouseExit (event);
}

//==============================================================================
void VelocityKnob::setDepth (float newDepth)
{
    processor.setModulationDepth (trackIndex, modTarget, juce::jlimit (-1.0f, 1.0f, newDepth));

    if (onDepthChanged != nullptr)
        onDepthChanged();

    repaint();
}

void VelocityKnob::refreshFromProcessor()
{
    const float velocity = processor.getLastVelocity (trackIndex);

    // An unmodulated knob draws no dot, so nothing can have moved. Tracking the velocity
    // anyway keeps the next comparison honest.
    if (std::abs (getDepth()) < 1.0e-6f)
    {
        displayedVelocity = velocity;
        return;
    }

    if (std::abs (velocity - displayedVelocity) < 1.0e-3f)
        return;

    displayedVelocity = velocity;
    repaint();
}

void VelocityKnob::showContextMenu()
{
    const bool hasDepth = std::abs (getDepth()) > 1.0e-6f;

    juce::PopupMenu menu;
    menu.addSectionHeader (juce::String (modTargetName (modTarget))
                             + "  -  track " + juce::String (trackIndex + 1));
    menu.addItem (1, "Reset velocity depth", hasDepth);
    menu.addSeparator();
    menu.addItem (2, "Clear modulation on this track");
    menu.addItem (3, "Clear modulation on all tracks");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                        [this] (int result)
                        {
                            if (result == 1)
                            {
                                setDepth (0.0f);
                            }
                            else if (result == 2)
                            {
                                for (int i = 0; i < kNumModTargets; ++i)
                                    processor.setModulationDepth (trackIndex, i, 0.0f);
                            }
                            else if (result == 3)
                            {
                                processor.getModulationState().clearAllDepths();
                            }
                            else
                            {
                                return;
                            }

                            if (auto* top = getTopLevelComponent())
                                top->repaint();
                        });
}

} // namespace df
