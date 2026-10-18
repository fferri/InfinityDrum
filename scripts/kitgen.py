#!/usr/bin/env python3
"""Generate InfinityDrum kits as .dfkit files.

    python3 scripts/kitgen.py <output-dir>

Each kit is an archetype - a musical idea expressed as an engine layout plus tuning,
character and dynamics - rather than a random point in parameter space. The lane defaults
below are the measured targets from the reference recordings, so what the generator varies
is the *idea*, and what it holds roughly constant is the lane's dynamic character.

Two constraints from the brief are hard-coded: track 1 always carries the main kick, and
tracks 7-8 always carry hats and cymbals.

Engine ids follow ModelId:  kick snare membrane metal clap cymbal fmPerc clang rumble
Distortion ids follow DistortionMode:  soft hard fold fuzz crush
Modulation targets follow ModTarget:  decay tone character sweep tune level drive pan send
"""

import os
import sys
import xml.sax.saxutils as xml_escape

MODELS = ["kick", "snare", "membrane", "metal", "clap", "cymbal", "fmPerc", "clang", "rumble"]

# The parameter's own range, from createParameterLayout. A kit that states a value outside
# this would be silently clamped by the plugin, so the file would lie about what plays.
LIMITS = {
    "level": (-60.0, 12.0), "pan": (-1.0, 1.0), "tune": (-24.0, 24.0),
    "decay": (0.0, 1.0), "tone": (0.0, 1.0), "character": (0.0, 1.0),
    "sweep": (0.0, 1.0), "drive": (0.0, 1.0), "send": (0.0, 1.0),
}

CLAMPED = []
DISTS = ["soft", "hard", "fold", "fuzz", "crush"]
TARGETS = ["Decay", "Tone", "Character", "Sweep", "Tune", "Level", "Drive", "Pan", "Send"]

#===============================================================================
# Lane defaults. drive and level are the two levers that set crest factor, which is
# what actually separates the lanes; tone sets the spectral tilt.
#===============================================================================
LANES = {
    "IN": dict(
        label="industrial",
        drive=0.55, dist="fold",
        tone=0.46, decay=0.45, character=0.62, sweep=0.55,
        send=0.07, level_kick=-6.0, level_base=-11.0, level_hat=-14.5,
        level_depth=0.10, tune=0.0,
        # Inharmonic offsets: industrial kits are not tuned to a scale.
        tuning=[0, -6, 5, 7, -3, 4, -9, 6],
        spread=(0.10, 0.18, 0.12, 0.16, -0.10, -0.20, 0.14, 0.18),
    ),
    "ID": dict(
        label="idm",
        drive=0.16, dist="soft",
        tone=0.62, decay=0.38, character=0.52, sweep=0.45,
        send=0.20, level_kick=-6.5, level_base=-10.5, level_hat=-13.0,
        level_depth=0.16, tune=0.0,
        tuning=[0, 0, 3, 7, 0, 5, -2, 4],
        spread=(-0.12, 0.10, 0.18, -0.16, 0.12, -0.08, 0.20, -0.14),
    ),
    "EL": dict(
        label="electronica",
        drive=0.24, dist="soft",
        tone=0.55, decay=0.45, character=0.50, sweep=0.50,
        send=0.22, level_kick=-6.0, level_base=-10.0, level_hat=-12.5,
        level_depth=0.14, tune=0.0,
        tuning=[0, 0, 7, 12, 0, 5, 3, 10],
        spread=(-0.05, 0.12, -0.10, 0.15, -0.14, 0.18, -0.08, 0.10),
    ),
    "AM": dict(
        # Ambient is defined by crest, so drive goes to nearly nothing and the levels sit
        # well clear of the master limiter. Reverb does the work instead.
        label="ambient",
        drive=0.02, dist="soft",
        tone=0.52, decay=0.78, character=0.55, sweep=0.40,
        send=0.48, level_kick=-4.0, level_base=-8.0, level_hat=-10.5,
        level_depth=0.12, tune=0.0,
        tuning=[0, 0, 5, 12, 7, 3, -5, 7],
        spread=(-0.30, 0.25, -0.20, 0.30, 0.35, -0.28, 0.22, -0.25),
    ),
}


def track(model, **overrides):
    """One track: an engine plus whatever this kit does differently from its lane."""
    spec = dict(model=model)
    spec.update(overrides)
    return spec


#===============================================================================
# The twenty archetypes.
#===============================================================================
KITS = [
    # ---------------------------------------------------------------- industrial
    dict(lane="IN", name="Scrapyard", idea="junkyard metal, everything struck and bent", layout=[
        track("kick",     decay=0.42, character=0.62, sweep=0.72, drive=0.60, dist="fold", tune=0),
        track("snare",    decay=0.40, tone=0.52, character=0.70, sweep=0.45, tune=-6, drive=0.55, dist="hard"),
        track("metal",    decay=0.14, tone=0.66, character=0.90, sweep=0.30, tune=5,  dist="crush"),
        track("metal",    decay=0.55, tone=0.60, character=0.88, sweep=0.24, tune=7,  dist="crush"),
        track("clap",     decay=0.42, tone=0.48, character=0.58, sweep=0.62, tune=-3, dist="fold"),
        track("rumble",   decay=0.48, tone=0.28, character=0.60, sweep=0.58, tune=-9, dist="fold"),
        track("clang",    decay=0.58, tone=0.58, character=0.80, sweep=0.48, tune=6,  dist="hard"),
        track("cymbal",   decay=0.62, tone=0.62, character=0.60, sweep=0.52, tune=4,  dist="crush"),
    ]),

    dict(lane="IN", name="Furnace", idea="low and slow, heat and drone", layout=[
        track("kick",     decay=0.72, tone=0.30, character=0.55, sweep=0.80, drive=0.62, dist="fold", tune=-5),
        track("membrane", decay=0.62, tone=0.34, character=0.62, sweep=0.40, tune=-10, drive=0.50, dist="fuzz"),
        track("rumble",   decay=0.70, tone=0.24, character=0.70, sweep=0.62, tune=-12, dist="fold"),
        track("rumble",   decay=0.50, tone=0.30, character=0.58, sweep=0.55, tune=-7),
        track("membrane", decay=0.45, tone=0.40, character=0.55, sweep=0.35, tune=-2),
        track("metal",    decay=0.30, tone=0.44, character=0.72, sweep=0.28, tune=3),
        track("clang",    decay=0.66, tone=0.42, character=0.74, sweep=0.44, tune=-4),
        track("cymbal",   decay=0.74, tone=0.44, character=0.66, sweep=0.58, tune=0),
    ]),

    dict(lane="IN", name="Rivet Gun", idea="dry, tight and mechanical", layout=[
        track("kick",     decay=0.22, tone=0.54, character=0.72, sweep=0.66, drive=0.58, dist="hard", tune=0),
        track("clap",     decay=0.18, tone=0.62, character=0.68, sweep=0.70, tune=2, dist="crush"),
        track("metal",    decay=0.08, tone=0.74, character=0.94, sweep=0.20, tune=9,  dist="crush"),
        track("fmPerc",   decay=0.12, tone=0.68, character=0.80, sweep=0.72, tune=8,  dist="hard"),
        track("fmPerc",   decay=0.26, tone=0.58, character=0.62, sweep=0.50, tune=-4),
        track("clang",    decay=0.34, tone=0.64, character=0.86, sweep=0.60, tune=11),
        track("metal",    decay=0.12, tone=0.70, character=0.88, sweep=0.26, tune=6),
        track("cymbal",   decay=0.30, tone=0.68, character=0.72, sweep=0.66, tune=7),
    ]),

    dict(lane="IN", name="Collapse", idea="dense and doom-laden, kick and sub together", layout=[
        track("kick",     decay=0.58, tone=0.34, character=0.60, sweep=0.76, drive=0.66, dist="fold", tune=-3),
        track("snare",    decay=0.52, tone=0.40, character=0.76, sweep=0.52, tune=-8, drive=0.58, dist="hard"),
        track("rumble",   decay=0.80, tone=0.22, character=0.66, sweep=0.55, tune=-12, dist="fold"),
        track("membrane", decay=0.66, tone=0.32, character=0.60, sweep=0.42, tune=-5, dist="fuzz"),
        track("clap",     decay=0.55, tone=0.42, character=0.62, sweep=0.68, tune=-4, dist="fold"),
        track("clang",    decay=0.70, tone=0.38, character=0.78, sweep=0.50, tune=-6),
        track("clang",    decay=0.44, tone=0.50, character=0.82, sweep=0.46, tune=5),
        track("cymbal",   decay=0.80, tone=0.40, character=0.68, sweep=0.60, tune=-2),
    ]),

    dict(lane="IN", name="Concrete", idea="blunt and mid-forward, no top end", layout=[
        track("kick",     decay=0.36, tone=0.40, character=0.68, sweep=0.60, drive=0.56, dist="hard", tune=0),
        track("snare",    decay=0.36, tone=0.44, character=0.74, sweep=0.40, tune=-4, drive=0.60, dist="crush"),
        track("membrane", decay=0.48, tone=0.36, character=0.58, sweep=0.38, tune=3,  dist="fold"),
        track("membrane", decay=0.34, tone=0.42, character=0.54, sweep=0.34, tune=8),
        track("clap",     decay=0.34, tone=0.46, character=0.60, sweep=0.58, tune=0),
        track("rumble",   decay=0.56, tone=0.26, character=0.64, sweep=0.50, tune=-7),
        track("metal",    decay=0.16, tone=0.48, character=0.80, sweep=0.24, tune=5),
        track("metal",    decay=0.40, tone=0.44, character=0.76, sweep=0.30, tune=1),
    ]),

    # ---------------------------------------------------------------------- idm
    dict(lane="ID", name="Sinew", idea="taut and sinewy, tuned percussion", layout=[
        track("kick",     decay=0.30, tone=0.60, character=0.58, sweep=0.62, dist="soft", tune=0),
        track("snare",    decay=0.28, tone=0.62, character=0.60, sweep=0.40, tune=3),
        track("metal",    decay=0.10, tone=0.72, character=0.86, sweep=0.28, tune=7),
        track("metal",    decay=0.34, tone=0.66, character=0.84, sweep=0.22, tune=12),
        track("fmPerc",   decay=0.26, tone=0.64, character=0.66, sweep=0.58, tune=5),
        track("fmPerc",   decay=0.44, tone=0.56, character=0.58, sweep=0.44, tune=-2),
        track("clang",    decay=0.52, tone=0.60, character=0.72, sweep=0.50, tune=4),
        track("cymbal",   decay=0.56, tone=0.66, character=0.62, sweep=0.54, tune=7),
    ]),

    dict(lane="ID", name="Halflife", idea="detuned and unstable, everything slightly wrong", layout=[
        track("membrane", decay=0.44, tone=0.52, character=0.62, sweep=0.54, tune=-4, drive=0.20),
        track("snare",    decay=0.36, tone=0.56, character=0.66, sweep=0.36, tune=6),
        track("metal",    decay=0.12, tone=0.68, character=0.88, sweep=0.26, tune=-9),
        track("fmPerc",   decay=0.40, tone=0.58, character=0.70, sweep=0.76, tune=11),
        track("fmPerc",   decay=0.22, tone=0.62, character=0.64, sweep=0.62, tune=-7),
        track("clang",    decay=0.60, tone=0.54, character=0.78, sweep=0.46, tune=13),
        track("clang",    decay=0.38, tone=0.58, character=0.74, sweep=0.42, tune=-11),
        track("cymbal",   decay=0.66, tone=0.60, character=0.70, sweep=0.58, tune=8),
    ]),

    dict(lane="ID", name="Glass Teeth", idea="bright and brittle, all edge", layout=[
        track("kick",     decay=0.20, tone=0.66, character=0.70, sweep=0.70, tune=5),
        track("clap",     decay=0.20, tone=0.74, character=0.72, sweep=0.66, tune=7),
        track("metal",    decay=0.07, tone=0.84, character=0.96, sweep=0.22, tune=14),
        track("metal",    decay=0.28, tone=0.80, character=0.92, sweep=0.18, tune=9),
        track("clang",    decay=0.44, tone=0.78, character=0.90, sweep=0.52, tune=16),
        track("fmPerc",   decay=0.18, tone=0.76, character=0.68, sweep=0.80, tune=12),
        track("clang",    decay=0.62, tone=0.74, character=0.88, sweep=0.48, tune=6),
        track("cymbal",   decay=0.58, tone=0.82, character=0.76, sweep=0.62, tune=10),
    ]),

    dict(lane="ID", name="Dowel", idea="dry wood and close mics, almost no tail", layout=[
        track("kick",     decay=0.16, tone=0.58, character=0.62, sweep=0.58, tune=-2),
        track("membrane", decay=0.20, tone=0.60, character=0.56, sweep=0.38, tune=5),
        track("membrane", decay=0.14, tone=0.66, character=0.60, sweep=0.32, tune=12),
        track("clap",     decay=0.12, tone=0.66, character=0.54, sweep=0.60, tune=0),
        track("snare",    decay=0.18, tone=0.62, character=0.62, sweep=0.34, tune=7),
        track("metal",    decay=0.05, tone=0.70, character=0.84, sweep=0.20, tune=3),
        track("metal",    decay=0.16, tone=0.64, character=0.80, sweep=0.24, tune=9),
        track("cymbal",   decay=0.24, tone=0.68, character=0.66, sweep=0.56, tune=5),
    ]),

    dict(lane="ID", name="Static Bloom", idea="textural and sparse, more atmosphere than beat", layout=[
        track("kick",     decay=0.34, tone=0.48, character=0.50, sweep=0.66, tune=-5),
        track("rumble",   decay=0.72, tone=0.34, character=0.68, sweep=0.50, tune=-10),
        track("clang",    decay=0.74, tone=0.56, character=0.82, sweep=0.54, tune=8),
        track("metal",    decay=0.20, tone=0.70, character=0.88, sweep=0.30, tune=-4),
        track("cymbal",   decay=0.84, tone=0.62, character=0.74, sweep=0.60, tune=3),
        track("fmPerc",   decay=0.56, tone=0.60, character=0.62, sweep=0.70, tune=15),
        track("clang",    decay=0.66, tone=0.52, character=0.76, sweep=0.44, tune=-8),
        track("cymbal",   decay=0.88, tone=0.58, character=0.72, sweep=0.66, tune=11),
    ]),

    # --------------------------------------------------------------- electronica
    dict(lane="EL", name="Cassette", idea="warm and slightly worn, tape saturation", layout=[
        track("kick",     decay=0.44, tone=0.42, character=0.56, sweep=0.62, drive=0.30, dist="soft", tune=0),
        track("snare",    decay=0.40, tone=0.52, character=0.58, sweep=0.38, tune=0),
        track("metal",    decay=0.12, tone=0.58, character=0.82, sweep=0.26, tune=7),
        track("metal",    decay=0.40, tone=0.56, character=0.80, sweep=0.22, tune=12),
        track("clap",     decay=0.38, tone=0.52, character=0.50, sweep=0.58, tune=0),
        track("membrane", decay=0.46, tone=0.46, character=0.52, sweep=0.34, tune=7),
        track("clang",    decay=0.34, tone=0.54, character=0.66, sweep=0.44, tune=5),
        track("cymbal",   decay=0.54, tone=0.56, character=0.60, sweep=0.56, tune=10),
    ]),

    dict(lane="EL", name="Marimba Room", idea="melodic tuned percussion, almost no drums", layout=[
        track("kick",     decay=0.26, tone=0.50, character=0.52, sweep=0.56, tune=3),
        track("membrane", decay=0.44, tone=0.56, character=0.48, sweep=0.36, tune=12),
        track("membrane", decay=0.36, tone=0.62, character=0.52, sweep=0.32, tune=12),
        track("fmPerc",   decay=0.40, tone=0.58, character=0.56, sweep=0.62, tune=5),
        track("fmPerc",   decay=0.30, tone=0.62, character=0.52, sweep=0.58, tune=7),
        track("clang",    decay=0.52, tone=0.56, character=0.64, sweep=0.48, tune=10),
        track("metal",    decay=0.14, tone=0.64, character=0.78, sweep=0.24, tune=10),
        track("cymbal",   decay=0.48, tone=0.60, character=0.58, sweep=0.52, tune=5),
    ]),

    dict(lane="EL", name="Night Bus", idea="deep and unhurried, late-night low end", layout=[
        track("kick",     decay=0.62, tone=0.30, character=0.54, sweep=0.68, tune=-7),
        track("snare",    decay=0.46, tone=0.40, character=0.54, sweep=0.36, tune=-5),
        track("rumble",   decay=0.58, tone=0.26, character=0.58, sweep=0.52, tune=-12),
        track("clap",     decay=0.44, tone=0.44, character=0.48, sweep=0.60, tune=-2),
        track("membrane", decay=0.52, tone=0.38, character=0.50, sweep=0.34, tune=-9),
        track("metal",    decay=0.12, tone=0.50, character=0.78, sweep=0.26, tune=4),
        track("clang",    decay=0.56, tone=0.44, character=0.68, sweep=0.46, tune=-6),
        track("cymbal",   decay=0.64, tone=0.46, character=0.62, sweep=0.54, tune=2),
    ]),

    dict(lane="EL", name="Paper Cup", idea="small, tight and close, a toy kit", layout=[
        track("kick",     decay=0.18, tone=0.56, character=0.60, sweep=0.60, tune=5),
        track("clap",     decay=0.14, tone=0.62, character=0.52, sweep=0.62, tune=0),
        track("metal",    decay=0.06, tone=0.72, character=0.86, sweep=0.22, tune=14),
        track("metal",    decay=0.22, tone=0.68, character=0.82, sweep=0.20, tune=9),
        track("membrane", decay=0.22, tone=0.60, character=0.48, sweep=0.30, tune=12),
        track("snare",    decay=0.20, tone=0.58, character=0.56, sweep=0.34, tune=7),
        track("clang",    decay=0.30, tone=0.64, character=0.70, sweep=0.42, tune=17),
        track("cymbal",   decay=0.34, tone=0.68, character=0.62, sweep=0.58, tune=12),
    ]),

    dict(lane="EL", name="Sodium", idea="neon bright, tuned metal and glass", layout=[
        track("kick",     decay=0.28, tone=0.60, character=0.58, sweep=0.64, tune=0),
        track("fmPerc",   decay=0.32, tone=0.68, character=0.60, sweep=0.66, tune=7),
        track("fmPerc",   decay=0.44, tone=0.64, character=0.64, sweep=0.72, tune=12),
        track("clang",    decay=0.50, tone=0.70, character=0.82, sweep=0.50, tune=10),
        track("metal",    decay=0.10, tone=0.76, character=0.90, sweep=0.24, tune=19),
        track("metal",    decay=0.30, tone=0.72, character=0.86, sweep=0.20, tune=10),
        track("clang",    decay=0.62, tone=0.66, character=0.80, sweep=0.46, tune=5),
        track("cymbal",   decay=0.56, tone=0.74, character=0.70, sweep=0.60, tune=14),
    ]),

    # ------------------------------------------------------------------ ambient
    dict(lane="AM", name="Drift", idea="slow washes, the kit as weather", layout=[
        track("membrane", decay=0.84, tone=0.44, character=0.50, sweep=0.44, tune=-5),
        track("membrane", decay=0.76, tone=0.48, character=0.46, sweep=0.38, tune=7),
        track("rumble",   decay=0.88, tone=0.34, character=0.56, sweep=0.50, tune=-12),
        track("clang",    decay=0.82, tone=0.52, character=0.62, sweep=0.46, tune=3),
        track("cymbal",   decay=0.92, tone=0.58, character=0.66, sweep=0.58, tune=10),
        track("fmPerc",   decay=0.70, tone=0.54, character=0.52, sweep=0.56, tune=15),
        track("clang",    decay=0.86, tone=0.50, character=0.60, sweep=0.42, tune=-8),
        track("cymbal",   decay=0.95, tone=0.54, character=0.64, sweep=0.62, tune=5),
    ]),

    dict(lane="AM", name="Ember", idea="warm haze, everything glowing and soft", layout=[
        track("kick",     decay=0.66, tone=0.34, character=0.42, sweep=0.52, tune=-3),
        track("membrane", decay=0.80, tone=0.42, character=0.48, sweep=0.40, tune=5),
        track("metal",    decay=0.20, tone=0.56, character=0.72, sweep=0.26, tune=7),
        track("clang",    decay=0.78, tone=0.48, character=0.66, sweep=0.44, tune=0),
        track("rumble",   decay=0.72, tone=0.30, character=0.54, sweep=0.48, tune=-9),
        track("fmPerc",   decay=0.74, tone=0.50, character=0.50, sweep=0.60, tune=12),
        track("clang",    decay=0.88, tone=0.46, character=0.62, sweep=0.40, tune=-4),
        track("cymbal",   decay=0.90, tone=0.50, character=0.60, sweep=0.56, tune=8),
    ]),

    dict(lane="AM", name="Glacier", idea="cold and bright, long glass tails", layout=[
        track("kick",     decay=0.40, tone=0.56, character=0.44, sweep=0.58, tune=0),
        track("clang",    decay=0.86, tone=0.68, character=0.74, sweep=0.48, tune=12),
        track("clang",    decay=0.80, tone=0.72, character=0.78, sweep=0.44, tune=-7),
        track("cymbal",   decay=0.94, tone=0.70, character=0.70, sweep=0.64, tune=7),
        track("metal",    decay=0.24, tone=0.78, character=0.84, sweep=0.28, tune=12),
        track("fmPerc",   decay=0.76, tone=0.66, character=0.56, sweep=0.62, tune=17),
        track("cymbal",   decay=0.96, tone=0.64, character=0.68, sweep=0.60, tune=5),
        track("metal",    decay=0.50, tone=0.74, character=0.80, sweep=0.22, tune=14),
    ]),

    dict(lane="AM", name="Sediment", idea="dark and sub-heavy, more felt than heard", layout=[
        track("kick",     decay=0.78, tone=0.22, character=0.48, sweep=0.62, tune=-9),
        track("rumble",   decay=0.92, tone=0.20, character=0.62, sweep=0.46, tune=-14),
        track("membrane", decay=0.74, tone=0.26, character=0.44, sweep=0.36, tune=-7),
        track("rumble",   decay=0.84, tone=0.24, character=0.58, sweep=0.42, tune=-3),
        track("membrane", decay=0.68, tone=0.30, character=0.40, sweep=0.32, tune=2),
        track("metal",    decay=0.18, tone=0.38, character=0.66, sweep=0.24, tune=-5),
        track("clang",    decay=0.80, tone=0.32, character=0.56, sweep=0.38, tune=-11),
        track("cymbal",   decay=0.86, tone=0.34, character=0.54, sweep=0.52, tune=-4),
    ]),

    dict(lane="AM", name="Tin Roof Rain", idea="scattered texture over a low foundation", layout=[
        track("kick",     decay=0.88, tone=0.22, character=0.38, sweep=0.34, tune=-7, drive=0.0),
        track("clang",    decay=0.60, tone=0.44, character=0.72, sweep=0.42, tune=9),
        track("clang",    decay=0.72, tone=0.48, character=0.76, sweep=0.40, tune=-6),
        track("metal",    decay=0.16, tone=0.50, character=0.86, sweep=0.24, tune=8),
        track("fmPerc",   decay=0.66, tone=0.42, character=0.58, sweep=0.58, tune=11),
        track("cymbal",   decay=0.88, tone=0.42, character=0.68, sweep=0.56, tune=2),
        track("cymbal",   decay=0.94, tone=0.46, character=0.72, sweep=0.62, tune=7),
        track("metal",    decay=0.44, tone=0.48, character=0.82, sweep=0.26, tune=13),
    ]),
]


def build_kit(spec):
    lane = LANES[spec["lane"]]
    layout = spec["layout"]

    if len(layout) != 8:
        raise ValueError(f"{spec['name']}: layout has {len(layout)} tracks, expected 8")

    tracks = []

    for index, entry in enumerate(layout):
        model = entry["model"]
        c = dict(lane)                       # lane defaults
        c.update(entry)                      # then this track's intent

        level = entry.get("level")
        if level is None:
            if index == 0:
                level = lane["level_kick"]
            elif index >= 6:
                level = lane["level_hat"]
            else:
                level = lane["level_base"]

        # Pan spreads the kit across the stereo field; hats and clangs go widest.
        pan = entry.get("pan", lane["spread"][index])

        # Tuning: lane offset plus this kit's chosen interval.
        tune = entry.get("tune", 0) + lane["tuning"][index] + lane["tune"]

        depths = {t: 0.0 for t in TARGETS}

        # Every kit gets the lane's velocity dynamics on level - that is what makes a kit
        # respond rather than sit flat.
        depths["Level"] = entry.get("level_depth", lane["level_depth"])

        # A little tasteful expression on top, varied per kit so the twenty do not all
        # respond identically.
        for target, amount in entry.get("depths", {}).items():
            depths[target] = amount

        fields = dict(
            level=level, pan=pan, tune=tune,
            decay=entry.get("decay", c["decay"]),
            tone=entry.get("tone", c["tone"]),
            character=entry.get("character", c["character"]),
            sweep=entry.get("sweep", c["sweep"]),
            drive=entry.get("drive", c["drive"]),
            send=entry.get("send", c["send"]),
        )

        for field, value in fields.items():
            lo, hi = LIMITS[field]
            if not lo <= value <= hi:
                CLAMPED.append(f"{spec['name']} track {index + 1}: {field} {value:+.2f} -> "
                               f"clamped to {min(max(value, lo), hi):+.2f}")
                fields[field] = min(max(value, lo), hi)

        tracks.append(dict(
            model=MODELS.index(model),
            dist=DISTS.index(c.get("dist", "soft")),
            **{k: round(v, 3) for k, v in fields.items()},
            depths=depths,
        ))

    return spec, tracks


def to_xml(name, tracks):
    lines = [f'<INFINITYDRUM_KIT name="{xml_escape.escape(name)}" version="1">']

    for index, t in enumerate(tracks):
        lines.append(
            f'  <TRACK index="{index}" model="{t["model"]}" dist="{t["dist"]}" '
            f'level="{t["level"]}" pan="{t["pan"]}" tune="{t["tune"]}" decay="{t["decay"]}" '
            f'tone="{t["tone"]}" character="{t["character"]}" sweep="{t["sweep"]}" '
            f'drive="{t["drive"]}" send="{t["send"]}">'
        )

        for target in TARGETS:
            depth = t["depths"][target]
            if abs(depth) > 1e-6:
                lines.append(f'    <MOD target="{target}" depth="{round(depth, 3)}"/>')

        lines.append("  </TRACK>")

    lines.append("</INFINITYDRUM_KIT>")
    return "\n".join(lines) + "\n"


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 1

    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)

    counters = {}
    for spec in KITS:
        lane = spec["lane"]
        counters[lane] = counters.get(lane, 0) + 1
        name = f"{lane}-{counters[lane]:02d} {spec['name']}"

        _, tracks = build_kit(spec)
        path = os.path.join(out, name.replace(" ", "_") + ".dfkit")

        with open(path, "w") as handle:
            handle.write(to_xml(name, tracks))

        print(f"{name:26} {spec['idea']}")

    if CLAMPED:
        print(f"\n{len(CLAMPED)} value(s) were outside the parameter range and got clamped:")
        for line in CLAMPED:
            print("  " + line)

    print(f"\n{len(KITS)} kits written to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
