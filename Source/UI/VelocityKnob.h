#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "../Core/DrumKitProcessor.h"
#include "Theme.h"
#include "UiState.h"

namespace df
{

/** A rotary slider that also edits, and draws, one parameter's velocity modulation depth.

    Painting lives here rather than in the shared LookAndFeel because the depth is per-knob
    state and a LookAndFeel instance is shared by every control in the editor.

    Gestures:
      drag the body   -> the parameter value (stock Slider behaviour)
      drag the rim    -> the velocity depth
      double-click    -> reset the value, or the depth when aimed at the rim
      right-click     -> reset / clear modulation

    On the MOD page the whole knob edits depth instead of value, so the target is large and
    obvious and the numeric readout switches to showing the depth.

    The dial is mostly dark: a bright value arc, a short radial pointer that ends where that
    arc does, and the velocity range as a band on an inner orbit. The glow and the band's end
    handles are hover affordances only - at rest the knob stays quiet, which matters with
    seventy-odd of them on screen.
*/
class VelocityKnob final : public juce::Slider
{
public:
    VelocityKnob (DrumKitProcessor& processor,
                  int trackIndex,
                  int modTarget,
                  EditorUiState& uiState);

    void setAccentColour (juce::Colour newAccent);

    /** Called from the row's timer. Repaints only when the drawn dot actually moved. */
    void refreshFromProcessor();

    float getDepth() const;

    /** False for global controls (master, reverb), which have no modulation target. */
    bool isModulatable() const noexcept { return trackIndex >= 0 && modTarget >= 0; }

    /** Fired whenever the depth changes, so the owning ModelKnob can redraw its readout. */
    std::function<void()> onDepthChanged;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    struct Geometry
    {
        juce::Point<float> centre;
        float radius = 0.0f;
        float arcRadius = 0.0f;
        float bodyRadius = 0.0f;
        float bandRadius = 0.0f;
        float bandWidth = 0.0f;
        float lineWidth = 0.0f;
        float startAngle = 0.0f;
        float endAngle = 0.0f;

        float angleFor (float normalised) const noexcept
        {
            return startAngle + normalised * (endAngle - startAngle);
        }

        juce::Point<float> pointAt (float normalised) const noexcept;
    };

    Geometry getGeometry() const;

    /** Normalised 0..1 position of the parameter's current value. */
    float getCentrePosition() const;

    bool isInRim (juce::Point<float> position) const;
    bool shouldEditDepth (const juce::MouseEvent& event) const;

    void setDepth (float newDepth);
    void showContextMenu();

    DrumKitProcessor& processor;
    const int trackIndex;
    const int modTarget;
    EditorUiState& uiState;

    juce::Colour accent { 0xffe8a33a };
    float displayedVelocity = 0.0f;
    float dragStartDepth = 0.0f;
    bool draggingDepth = false;
    bool hovering = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VelocityKnob)
};

} // namespace df
