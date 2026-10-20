# InfinityDrum architecture

How the plugin is built and why it is built that way. For *using* InfinityDrum see
[README.md](README.md); for *changing* it, including the golden rules, see
[AGENTS.md](AGENTS.md).

---

## The shape of it

One `AudioProcessor` (`DrumKitProcessor`) owns eight `DrumTrack`s and a shared reverb. Each
track owns **every synthesis engine at once** and a pointer to whichever is active.
Everything below the UI is plain C++ with no allocation on the audio thread.

The plugin is deliberately small in concept: eight tracks, each a triggerable sound with a
handful of parameters, plus a bus. The complexity lives in the engines, and in the decision
to make velocity a first-class modulation source rather than a hard-coded loudness curve.

---

## Source layout

```
Source/
  Core/
    DspUtils.h            RNG, polyBLEP, TPT state-variable filter, DC blocker,
                          wavefolder, the five distortion curves
    Envelopes.h           percussive AD envelope + exponential decay contour
    DrumModel.h           ModelId, ModelInfo, ModelParams, engine base class
    Models/               one file per engine, plus the factory and metadata table
    VelocityModulation.h  the velocity curve and how it moves a parameter
    ModulationState.*     per-parameter depths: saved with the session, not automatable
    KitLibrary.*          kits as data: capture, apply, XML, and the factory + user library
    DrumTrack.h           one track: engine hosting, per-track FX, metering
    Parameters.h/.cpp     APVTS layout, note maps, the factory kit table
    FactoryKits.inc       generated: the kit table, from kits/*.dfkit
    DrumKitProcessor.*    the AudioProcessor: 8 tracks, MIDI, reverb bus, master
    Settings.*            global preferences, kept out of the session
  UI/
    UiState.h             editor-wide state the knobs need on every paint (MOD armed)
    Theme.*               look and feel for everything except the knobs
    VelocityKnob.*        the rotary itself: band, last-hit dot, rim drag, context menu
    ModelKnob.*           velocity knob + live-relabelling caption + value readout
    TrackStrip.*          pad, engine + distortion selectors, knobs, mute/solo, meter
    DrumKitEditor.*       window layout, kit selector, MOD toggle
  PluginEntry.cpp         createPluginFilter()
```

---

## The engines

Each engine is a distinct synthesis technique rather than a preset of a common core. They
all derive from `DrumModel` and share the same four macro parameters, but each interprets
those macros in its own way.

| Engine | Technique |
|---|---|
| **Kick** | Pitch-swept sine plus a filtered noise beater click |
| **Snare** | Three decaying shell modes plus band-limited noise wires |
| **Membrane** | Six modal partials, swept between harmonic and circular-membrane ratios |
| **Metal** | Six band-limited squares at inharmonic ratios, heavily filtered |
| **Clap** | Band-passed noise gated by a retriggering staircase envelope |
| **Cymbal** | Four ring-modulated oscillator pairs plus a stick-attack burst |
| **FM Perc** | Two-operator phase modulation with a decaying index |
| **Clang** | Inharmonic FM, ring-modulated against a third oscillator |
| **Rumble** | Two detuned subs plus a low-passed noise bed, up to twelve seconds |

Several borrow a specific technique worth naming:

- **Metal** uses the TR-808 hi-hat topology: six band-limited squares at deliberately
  inharmonic ratios — `2.0, 3.0, 4.16, 5.43, 6.79, 8.21` — heavily high-passed so that only
  their upper harmonics survive.
- **Cymbal** is four ring-modulated oscillator pairs. That is a cheap and very effective way
  to build the dense inharmonic spectrum of a crash or ride without the numerical fragility
  of extremely high-Q resonators.
- **Membrane** sweeps between harmonic partials and the roots of the Bessel function J₀ —
  `1, 1.5934, 2.1355, 2.2958, 2.6531, 2.9173`, the ideal circular membrane's mode ratios.
  That sweep is what moves it from a tom towards a woodblock.
- **Clap** is a single envelope re-opened by several rapid bursts; the last burst is the one
  that decays into the tail.

Two engines exist for a specific reason:

- **Clang** is the inharmonic engine. Its modulator and ring ratios move between √2, π, e,
  √5 and the golden ratio — never whole numbers — so its partials cannot align with the
  fundamental at any tuning. It is atonal by construction rather than by detuning. The
  `Clang` macro moves those ratios further from consonance *and* raises the FM index, taking
  it from a struck bell towards torn metal. The ring modulation is what turns the FM
  sidebands into a struck-metal spectrum: every partial spawns a sum and a difference pair.
- **Rumble** supplies low end. Two sub oscillators a fraction of a percent apart beat slowly
  against each other, which is what gives long kick tails their movement. Decay reaches
  twelve seconds, so it doubles as a sustained bed. Through `Fold` drive it reads as part of
  the kit's body rather than a separate instrument.

The macro labels are per-engine, and `ModelFactory.h` carries the table that the UI reads to
relabel the knobs live. That table is the single place the UI and the engines agree on what
a macro means.

---

## Signal flow

```
MIDI or pad click
      |
      v
DrumTrack::trigger  ->  active engine renders into a mono buffer
      |
      v
de-click ramp  ->  DC blocker  ->  distortion (drive + curve)
      |
      v
pan + level  ->  stereo mix bus,  plus a post-fader send to the reverb
      |
      v
master gain  ->  soft clipper  ->  output
```

### Shared DSP building blocks

Decisions in `DspUtils.h` and `Envelopes.h` that everything else depends on:

- **Decay is a −60 dB time.** `kDecayToTau = ln(1000)` converts "time to fall 60 dB" into an
  exponential time constant, so the Decay knob means the same thing on every engine rather
  than each one inventing its own coefficient.
- **Knob ranges are exponential where the ear expects it** (`expMap`), giving a constant
  ratio per unit of travel instead of a constant increment.
- **Denormals are flushed** (`flushDenormal`), which guards against denormal stalls creeping
  into feedback paths.
- **`fastTanh` is a Padé 3/2 approximation**, continuous at the ±3 clamp and never exceeding
  1 — so no distortion curve can overshoot.
- **Envelopes snap to zero below −100 dB** and report inactive, so idle voices cost nothing.
- **Oscillators are PolyBLEP band-limited**, which removes the aliasing a naive square would
  fold back.
- **The filter is a TPT state-variable filter** (Cytomic/Zavalishin form): stable under
  modulation and well behaved at high cutoff. `q = 0.5` is critically damped.
- **A DC blocker** removes the offset that asymmetric waveshaping introduces.

### Retrigger de-clicking

Hitting a track while it is still ringing would otherwise cut the waveform and click. The
track fades its **own** engine output out over 1.5 ms underneath the new hit.

The ordering matters and was got wrong once: the fade is applied **before** the DC blocker,
so every filter downstream stays continuous. Applying it after the blocker instead let the
blocker answer the step with a low-frequency thump — measured at 0.079, five times larger
than the click it was supposed to remove. Before the blocker the same step measures 0.0017.

### Distortion

Five curves (`soft`, `hard`, `fold`, `fuzz`, `crush`) with level compensation, so raising
Drive changes character rather than loudness. The make-up divisors were tuned so the five
land within 0.8–1.1 dB of each other at full Drive; the first attempt was 5–9 dB quieter
than clean. Drive at 0 is a true bypass.

Note that the compensation means **the distortion does not reduce crest factor**. That
matters for the kit design process below: drive is a character control, not a compressor.

### The master bus

A `tanh` soft clipper guarantees the output never exceeds full scale however densely the kit
is played, and the default master level is −5 dB.

The reverb is JUCE's Freeverb on a shared bus fed post-fader by each track's `Send`. Its tail
comes back well below the level of what fed it, so the return carries makeup gain
(`reverbReturnGain = 2.0f`) — without it the reverb was inaudible at sensible settings.

Note that the send is applied **post-fader but not fader-squared**: the track's output
already carries the level, so scaling the send by the fader again would double-apply it. That
was a real bug, caught by checking that a 20 dB fader cut produced a 20 dB send cut rather
than 40.

**There is deliberately no compressor or limiter on the master.** Reference recordings are
mastered, so their crest factor is not achievable by an instrument that does not master
itself, and chasing it would mean baking a mastering stage into a plugin. See *Kit design*
below for what that implies for the factory kits.

---

## Real-time constraints

- **No allocation or locking on the audio thread.** Every engine is instantiated per
  track up front, so changing a track's engine just retargets a pointer.
- **UI pad clicks reach the audio thread through a lock-free `AbstractFifo`**, not a lock.
- **Everything the audio thread reads is either a parameter pointer or an atomic.** Depths,
  the note map and the last-hit velocity are all mirrored into atomics.
- **`prepareToPlay` is re-entrant.** Hosts call it repeatedly on buffer-size changes and
  device re-activation. The track detaches its active-engine pointer before replacing the
  engine array, so a second call cannot dereference freed memory.

  This was a real crash: Ableton Live failed with `EXC_BAD_ACCESS` in `DrumTrack::reset()`
  on the *second* `prepareToPlay`, because `activeEngine` still pointed into the array that
  `prepare()` had just replaced. It only reproduced on a second call, which is why it
  survived the first round of testing.

---

## Parameters

`createParameterLayout()` builds the whole APVTS: eleven parameters per track plus the kit
selector, note map, master and four reverb controls. `cacheParameterPointers()` caches the
raw `std::atomic<float>*` for each, so the audio thread never does a string lookup.

Two behaviours worth knowing:

- **Latch-at-trigger.** A new hit adopts the knob positions immediately rather than gliding
  towards them, so a hit sounds like the knob looks. Moves made *during* a ring are still
  smoothed.
- **Two smoothing rates.** Block-rate for engine parameters, per-sample for
  level/pan/drive/send.

---

## Velocity modulation

This is the largest design decision in the project, and parts of it are counterintuitive.

### The curve

Bipolar and symmetric about **velocity 100**, the value most controllers send by default:
127 maps to +1, 100 to 0, 0 to −1. So a nominal hit plays every knob exactly as dialled
whatever the depths are, and depth 0 leaves a parameter bit-for-bit untouched at any
velocity.

Modulation is applied in **normalised parameter space**, not value space. That means a depth
of 0.1 is a tenth of the knob's travel whether the knob is in dB, semitones or a 0..1 macro,
and it is what makes the drawn band symmetric about the value.

Implementing the "nominal hit is bit-identical" promise exactly required an early return
when the bipolar weight is zero. Without it, even a zero offset was pushed through
`convertTo0to1`/`convertFrom0to1`, and that round trip is not exactly lossless — so the knob
was being silently perturbed at velocity 100, which is precisely the case the design claims
is untouched. A test asserting bit-identity caught it.

### What it replaced

Four hard-coded velocity responses used to live inside the engines: a fixed loudness curve
on all nine, `velocityBrightness` on Membrane only, and velocity-folded-into-index on Clang
and FM Perc only. They covered exactly the four macro slots, but only on the engines where
someone had thought to add them.

Velocity is now removed from `DrumModel::trigger` entirely. It enters the system in exactly
one place — as already-modulated parameter values — which is what makes the behaviour
adjustable instead of compiled in.

### Why depths are not parameters

Depth is deliberately **not** an `AudioProcessorParameter`. Automating a knob and automating
velocity already spans the space; depth is only the slope between them, which is the part
that should stay static. Keeping them out also avoids adding 72 entries to the host's
automation view for no gain.

They still have to survive a save, so they live as properties on a `MODULATION` child of the
APVTS's own tree. `copyState()` deep-copies and `replaceState()` preserves extra children, so
the on-disk format is unchanged and a session saved before the feature existed decodes as
"no modulation". The audio thread never touches that tree; every depth is mirrored into an
atomic and re-mirrored after the host replaces our state.

---

## Kits, presets and persistence

### Kits are data

A kit is 8 tracks × 11 parameters plus nine modulation depths each — the complete state
needed to reproduce a sound. `KitData` is that, and `captureKit`/`applyKit` move it in and
out of a processor.

`scripts/kitgen.py` holds the archetypes for twenty of the kits and writes `kits/*.dfkit`;
`scripts/emit_factory_kits.py` compiles every `.dfkit` into `Source/Core/FactoryKits.inc`,
which `Parameters.cpp` includes inside its table. `kits/` is the source of truth and the
`.inc` is generated.

Three things this pipeline enforces:

- **A kit file cannot state a value the plugin would clamp.** `kitgen.py` clamps to the real
  parameter ranges and reports what it clamped. Four kits had tunings past +24 semitones and
  were silently clamped — worse, four tracks in one kit all landed on the same ceiling,
  collapsing a melodic spread onto one note.
- **The emitter rejects an unknown modulation target** rather than dropping it.
- **Engine and curve indices are emitted symbolically** (`mKick`, `dFold`) so the generated
  table still reads like a hand-written one in a diff.

### Built-in kits live in the binary

They are not written to disk on first run. Files on disk would be frozen copies, so improving
a kit in a later release would leave users with a stale version and a name clash, and
"restore the factory kits" would have to exist purely to undo what first run did. Users'
own kits are files, because those are content they may want to back up or share.

The three original kits keep indices 0–2 so a session that saved "kit 1" still recalls
Warehouse.

### Loading a kit replaces everything

`applyKit` used to take a `DepthPolicy` with a `levelOnly` mode that left the player's tone
depths alone. That existed only because the factory table could not carry more than a level
depth. Now that it carries all nine, partially applying a kit would just mean quietly losing
whatever its author dialled in, so the concept is gone: a kit is a complete description.

---

## Note maps

Three maps are supported, and the first two genuinely conflict — note 39 is a hand clap in
General MIDI and the first pad on a BeatStep Pro. So it is a switch, not a union, and each map
ignores the others' notes.

The third, **chromatic**, exists because a plain ascending run of eight notes is the only thing
some controllers send. It is MIDI 60–67: "C3" is what the UI calls middle C, because note names
are rendered with middle C in octave 3. That choice is a convention, not a fact — a DAW that
calls middle C "C4" will show these notes an octave lower — so the README states the MIDI
numbers alongside the names rather than relying on the label alone.

The map is a **real parameter**, so it is saved with the session and visible to the host. Its
*default* comes from a global preference file, so a one-time hardware choice applies to every
new instance without re-picking it per project.

That combination is not obvious, and it was arrived at the hard way. The first version was
global-only, which meant the plugin's MIDI behaviour depended on a file the host knew nothing
about. The host-load test — which asserted that GM notes 36 and 38 make a sound — then failed
on a machine where the preference had been set to BeatStep, because they correctly make no
sound at all. The plugin was right and the test was wrong, but a test that passes or fails
depending on whose Mac it runs on is not a test.

---

## The sequencer

`Source/Core/Sequencer.*` holds the model and the transport; `Source/UI/SequencerLane.*` is
the editor for one lane. The two are deliberately separate: the transport is testable with no
UI, and the UI touches only the plain struct.

### The data model

Eight `SequencerLane`s, each with its own type, speed and length. Nothing is shared, which is
what makes polymeters possible — a 5-step grid against a 7-step grid with the same speed
drifts and realigns on its own.

**Speed is a multiplier of a 1/16 note**, so `stepBeats = 0.25 / multiplier`. Bigger is
faster. This is the opposite of the intuitive reading of the numbers ("1/8 is smaller than
1"), and it was got wrong once: the first implementation treated the values as step lengths
in beats, which made ×8 eight times *slower* rather than eight times faster. The tell is
that ×1/3 has to mean a 3/16 step for triplets to work against a straight lane.

### Euclidean rhythms

`euclideanStepOn(step, pulses, steps)` is the closed form `(step * pulses) % steps < pulses`,
which agrees with Bjorklund for the evenly-distributed cases without the recursion. E(3,8)
gives `x..x..x.`.

### The transport

`SequencerTransport::advance()` is called once per block with the host's quarter-note position
and returns one `LaneAction` per lane. Three rules matter:

- **The first block never fires.** It adopts the current step index instead, so pressing play
  mid-bar does not dump a step into the buffer.
- **A jump of more than four steps between blocks is a seek.** It re-adopts the position
  rather than firing everything it passed over. Four is far beyond what any block can advance
  even at ×8, so it cannot be confused with normal playback.
- **A stopped transport holds the lanes.** Nothing fires, and a modulation lane keeps
  reporting its value so an automated parameter does not snap back to its knob position the
  moment the transport stops.

Triggers are sample-accurate. The transport does not fire anything itself: it returns a list
of `TriggerEvent`s, each carrying the sample offset it belongs at, and `processBlock` merges
that list with the MIDI stream and calls `renderRange` up to each event before firing it. That
loop already existed for MIDI, so the sequencer joined it rather than duplicating it.

Enumerating boundaries is also a correctness fix, not only a timing one. The earlier version
compared the current step with the previous one and fired at most once per block, so a block
spanning two boundaries silently dropped the first. At x8 (a 1/128 note, 15.6 ms) and a
1024-sample buffer that is every other step failing to play.

One trap came out of this: `getSampleRate()` is zero until a host calls
`setRateAndBufferSizeDetails`, which hosts do but the offline tools do not. The transport
divides by the rate to convert beats to samples, so a zero rate turned one block into
thousands of beats and a lane fired until it hit the event cap. `prepareToPlay` now records
the rate if it is not already set.

### Modulation lanes

The envelope is evaluated at the current cycle phase and scaled by the lane's `amount`, giving
an offset in *normalised* parameter space. It is added to the velocity offset inside
`applyVelocityModulation`, which already works in that space, so the two sources compose
without either one having to know about the other. The clamp keeps either from pushing the
other past the end of the range.

The bit-identical guarantee still holds for the case it was written for: a nominal hit with
no sequencer contribution returns the knob's value untouched.

### Persistence

The lanes are not parameters, so they ride in the APVTS tree as a `SEQUENCER` child, the same
way the modulation depths do. `getStateInformation` copies the tree, replaces that child and
serialises; `setStateInformation` reads it back after `replaceState`.

Step patterns are stored as a 64-character bit string rather than 64 properties: compact, and
it reads as a pattern when someone opens a project file in an editor. Breakpoints are encoded
as `position:value;...` with the count implied.

A session saved before lanes existed has no such node. That decodes as the defaults rather
than leaving whatever the instance happened to be holding, which is the same rule the
modulation depths follow.

**Lanes are not part of a kit.** A kit is a sound; a pattern is an arrangement. Loading a kit
must not overwrite the groove on the page.

### Threading

The lane struct is written on the message thread (the page) and read on the audio thread
(the transport) with no lock. Every field is a plain scalar, so the worst a race can do is
let one block see a half-applied edit — a step early or late, never a crash or a wedged
voice. A snapshot-buffer scheme would be stricter, but it would also be a lot of machinery
for state that is edited at most once per mouse event.

### Testing

The offline harness has no host, so `getPlayHead()` returns null and nothing would ever run.
`TestPlayHead` in `RenderTests.cpp` supplies a transport the tests advance by hand. Without
it the sequencer's audio path would be entirely untested — which is exactly the kind of gap
that ships a sequencer that does nothing.

A test that checks "the stopped transport fires nothing" initially failed because it was
measuring the previous hit still ringing. It now renders past the decay first.

## The UI

### The knobs paint themselves

`InfinityDrumLookAndFeel` deliberately does **not** implement `drawRotarySlider`. A LookAndFeel
instance serves every control in the editor, so it cannot hold per-knob state like a
modulation depth. `VelocityKnob` is a `Slider` subclass that draws its own band, dot and
pointer, and handles rim-drag for depth.

### The band and the audio share one implementation

Both the editor's band and the audio path's modulation go through `velocityBand()` and the
same normalised-space maths, so what is drawn cannot drift from what is heard. The band is
coloured by *velocity* rather than by screen position, which is what makes a negative depth
visibly reverse the gradient instead of merely sliding it.

### Pages, and the main menu

Three page tabs — SYNTH, MOD, SEQ — switch what the content area shows. The mode lives in
`EditorUiState` as an atomic, not a parameter: it changes how a drag is interpreted and what
is drawn, and nothing about it belongs to the sound. `VelocityKnob` and `ModelKnob` read it on
every paint and mouse event, which is why it sits in the shared state object rather than being
found by walking up the parent chain.

Only SYNTH and MOD have content today. SEQ is deliberately a labelled placeholder rather than
an empty panel: the page exists so the shape of the instrument is settled, and saying so is
more honest than leaving a blank rectangle that looks broken.

Everything that applies to the whole instrument moved into the main menu — a drawn hamburger
button at the top right — rather than staying on the tracks:

- **Kits**, with load, save, import, export and delete. Import and export go through a
  `FileChooser` and really are file operations; import also adds the kit to your library.
- **The MIDI note map.**

The note map used to hang off a right-click on a track pad, on the reasoning that the pad is
what responds to it. That was wrong once the map became one of three: it is a property of the
whole instrument, and three tracks sharing a setting should not have it tucked behind one of
them.

The menu button is drawn rather than set as text, because the "hamburger" character is missing
from some fonts and a tofu box would be worse than no icon. It is a `Button` subclass in
`Theme.h`, which is also where the styling that used to live only on text buttons now applies.

### Layout constraints

The editor allows 1120×720 up to 2400×1500. The eight tracks are **rows**, not columns, so the
knobs are laid out across each row and absorb whatever width the fixed blocks do not use. That
is why the row tolerates a narrow window: the identity block, pad, two selectors, mute/solo,
meter and `RND` are fixed or square, and the nine knobs divide the remainder.

Switching from columns to rows cost vertical space per track, so the minimum window height went
from 660 to 720 to keep the knob captions legible.

Smaller decisions, each of which was made for a reason:

- **A rim drag covers 180 px for a full −1..+1 depth sweep.** Fast enough to be useful, slow
  enough to place a value precisely.
- **A depth drag is deliberately not forwarded to the `Slider`**, so the parameter value
  cannot move at the same time.
- **The dot uses JUCE's rotary-thumb angle convention**, so it sits on the arc exactly where
  the pointer does, including on a skewed range.
- **Depth handles are drawn on top of the value arc**, so the arc cannot bury the thing you
  are meant to grab.
- **The band is drawn wide but sheer** — wide enough to read as a range, translucent enough
  that the crisp value arc on top still wins.
- **The level meter floors at −60 dB** and normalises over `(dB + 48) / 54`, going hot above
  0.86.
- **The editor ticks at 30 Hz**, enough for meters and pad glow without repainting the window
  more often than it changes.
- **Tooltips carry the policy, not just the control.** The kit button explains why built-in
  kits cannot be edited, and `RND` says what it will not touch, because those are the two
  places a user is most likely to be surprised.

### Testing seams

`DrumKitEditor` exposes `refreshFromProcessor()` and `getTrackNoteLabel()` purely so a test
can drive one editor tick and assert what a track header reads. That seam exists because of a
specific bug: the code that refreshes the note label landed in the `TrackStrip` constructor
instead of the timer callback — both begin with the same line — so the label was correct when
the editor opened and frozen thereafter.

---

## The randomiser

`RND` rolls new settings for one machine. It is **constrained, not uniform**, because a
uniform draw over nine parameters mostly produces mud. Each engine carries a sweet-spot range
per macro in a table in `Randomizer.cpp`, taken from the ranges the factory kits actually
live in.

Two things it refuses to touch:

- **The engine.** It reads the current engine and never writes the model parameter. Choosing
  an engine is part of setting a track up; the useful gesture is "vary the machine I have".
- **Level, mute and solo.** Mix decisions, and changing them would undo the balance of the
  kit you are working in.

Tuning draws from musical intervals rather than freely, so a rolled machine cannot beat
against the rest of the kit.

---

## Kit design

The twenty-five factory kits were designed against reference recordings rather than by
ear alone, using `InfinityDrumKitLab` and `scripts/kitmetrics.py`.

The most useful finding was that the four lanes are separated by **dynamics, not timbre**:

| Lane | Crest factor | Spectral tilt |
|---|---|---|
| industrial | +9.9 dB | −24.5 dB/decade |
| electronica | +12.3 dB | −19.3 |
| IDM | +16.5 dB | −18.3 |
| ambient | +19.5 dB | −19.0 |

Crest factor orders the lanes monotonically; tilt does not separate them at all. So kit design
varies drive, limiting headroom and density per lane, and leaves timbre free within a lane.

Two things follow that are easy to get wrong:

- **Patterns matter as much as kits.** Crest is dominated by how many hits land per bar, so
  each lane is rendered through a pattern whose density matches its genre. Rendering a sparse
  ambient kit through a four-on-the-floor pattern would push it into the limiter and make it
  measure like an industrial kit.
- **Crest is not a per-kit target.** The lever response was measured rather than assumed:
  +0.4 drive moved crest by 0.1 dB, and track level bought only ~2.5 dB for +9 dB because it
  works through the master limiter. What actually moves crest is how much hits overlap, so a
  short dry kit and a long washy kit in the *same* lane differ more than two different lanes
  do. Crest in a reference track is a property of arrangement and mastering, so it is
  advisory in the reports; the gate is spectral tilt plus level sanity.

---

## Build and packaging

JUCE is **not vendored**. It is cloned as a subdirectory, which is why `JUCE/` is gitignored
and why CI caches it — fetching it takes longer than the build.

Everything except the entry point lives in a static library (`InfinityDrumCore`) so the offline
tools can reuse it. `PluginEntry.cpp` is deliberately its own translation unit, compiled
straight into the plugin target, so the wrapper can always find `createPluginFilter`
regardless of how the linker treats the static library.

`InfinityDrumHostTest` deliberately does **not** link `InfinityDrumCore`: it exercises the shipped
`.vst3` as a black box, through the same hosting code a DAW uses, so it must not be able to
reach into the plugin's internals.

### Two platform traps worth knowing

- **A `gui_app` on Windows links `/SUBSYSTEM:WINDOWS`, which needs `WinMain`.** Only a
  `JUCEApplication` built with `START_JUCE_APPLICATION` provides one. `InfinityDrumScreenshot`
  had a plain `main()`, so it failed to link there; it is a `console_app` now, which is also
  the honest description of a command-line tool that renders offscreen. `HostLoadTest`, by
  contrast, must be a `gui_app`: on macOS JUCE drives the message loop with `[NSApp run]`,
  which only works from a real app bundle.
- **Failure reporting in CI.** GitHub returns `403` for job logs unless you have repository
  admin rights, which makes a CI-only failure invisible. The workflow emits `::error::` lines
  instead, because GitHub turns those into check-run annotations and *annotations* are
  readable without admin rights. That is also how the Windows-only test failure was found.

### Audio Unit

The macOS build produces an AU alongside the VST3. JUCE gates the AU format on the platform
(`_juce_get_platform_plugin_kinds` only returns it for Darwin), so the Windows build simply
produces the VST3 and no conditional is needed in `CMakeLists.txt`.

The AU needed the same re-sign as the VST3, for a different reason: CMake signs the bundle
at link time, and the AU's `Info.plist` appears after that, so the signature described a
bundle that no longer matched. Both bundles now go through one `foreach` that signs and
verifies after their post-build steps have run.

CI runs `auval` on the built AU. That is Apple's own validator and the only thing that
proves an AU actually loads in a host - a clean `codesign` says nothing about whether the
component registers, renders or handles MIDI.

### Code signing on macOS

CMake signs the bundle as part of *linking*, but JUCE writes
`Contents/Resources/moduleinfo.json` in a `POST_BUILD` step that runs afterwards. A fresh
build therefore ships a signature that does not seal that file, and macOS reports
`a sealed resource is missing or invalid` — an invalid signature, not merely an ad-hoc one.

The fix is a `POST_BUILD` command registered in `CMakeLists.txt` **after** `juce_add_plugin`.
Custom commands on a target run in the order they were added, so it runs after JUCE's own and
seals whatever is in the bundle by then. It then runs `codesign --verify`, so a regression
fails the build rather than shipping.

This hid for several releases because an **incremental** build re-signs the bundle after
`moduleinfo.json` already exists, so it verified locally while every clean build in CI, and
every download, did not. Deleting the bundle and rebuilding reproduced it immediately. That
is the reason `AGENTS.md` insists on testing the path CI actually runs.

### Why the packages are not notarised

Notarisation needs a paid Apple Developer account. A self-signed certificate does not help:
the build is already a valid signature — `codesign --verify` reports "valid on disk" — and
`spctl -a` still answers `rejected`, because Gatekeeper wants a signature chaining to Apple's
Developer ID rather than merely a valid one. The only way a self-signed certificate could
change that is if users trusted it as a root, which is not a reasonable thing to ask.

So the macOS package ships an install script that clears the quarantine flag instead. Windows
has the equivalent problem in SmartScreen, which a self-signed certificate does not solve
either.

---

## Testing

Four tools, all of which exit non-zero on failure and run in CI. What each one covers:

| Tool | Covers |
|---|---|
| `InfinityDrumTests` (433 checks) | Everything headless: engines, level bounds, decay to true silence, sample rates, de-clicking, choke group, the velocity curve and its clamping, depth persistence, kit capture/restore, the randomiser's constraints, note maps, XML and filesystem round trips, the full host lifecycle |
| `InfinityDrumHostTest` (23 checks) | End-to-end through JUCE's real VST3 hosting code — the same path a DAW takes: bus layout, MIDI, parameter exposure, rendering, editor construction and painting, state save/restore, prepare/release cycles |
| `InfinityDrumScreenshot` | Editor layout and painting at four window sizes. The only way to check the band and dot actually draw |
| `InfinityDrumRender` | Writes WAVs for listening. Not an assertion, but the quickest way to hear a regression |

### What is not covered

- **The editor's mouse gestures.** Rim drag, MOD drag and the right-click menu have no
  automated coverage; only the maths behind them does.
- **The `InfinityDrumScreenshot` output is not compared against a reference image.** It would
  catch more, but image comparison is brittle across macOS versions.

### Two rules that exist because of specific bugs

- **Offline tools pin their settings directory before constructing a processor.** They build
  their note events from General MIDI numbers, so picking up a user's BeatStep preference
  silently ignored most of them and made the output depend on the machine. That invalidated a
  round of kit measurements before it was noticed.
- **Anything reading a user preference must not write to it during tests.** A test that
  silently changed the note map of whoever ran it would be a genuinely nasty bug to chase.

---

## Invariants

Rules the code depends on being true. Each of these has already been broken once and caused a
bug, which is why they are written down rather than left implicit.

### Real-time safety

1. **Everything on `DrumTrack` is audio-thread only**, except `readPeakAndReset` and
   `getTriggerCount`. The UI communicates through those atomics and the APVTS, never by
   touching engine state directly.
2. **`setModel` must not allocate.** Retargeting a pointer is the entire reason every engine is
   instantiated up front.
3. **The audio thread never touches the APVTS ValueTree.** It is not thread-safe; depths and
   the last-hit velocity are read from atomics instead.
4. **Parameter writes go through `setParameterValue`**, which notifies the host, and are
   message-thread only.
5. **`readTrackPeak` is read-and-clear.** Call it once per observation — never twice in one
   expression, because C++ leaves argument evaluation order unspecified and the second call
   returns zero.

### Signal path

6. **The retrigger de-click stays before the DC blocker.** Moving it after makes the blocker
   answer the step with a thump far larger than the click.
7. **Mute and solo are applied after modulation**, so a muted track is silent whatever velocity
   arrives.
8. **The FX send is post-fader and is never scaled by the fader a second time.**

### Lifecycle

9. **`prepareToPlay` must null `activeEngine` before replacing the engine array.** Hosts call it
   repeatedly, and a dangling pointer there crashed Ableton Live on the second call.
10. **Rendering must chunk blocks larger than the prepared size.** Hosts do not always hand over
    the block size they prepared with.

### Modulation and kits

11. **Nominal velocity and depth 0 must return the knob value bit-for-bit.** That is why
    `applyVelocityModulation` returns early on a zero weight: the normalised round trip is not
    lossless.
12. **Depths must never become `AudioProcessorParameter`s**, or they appear in the host's
    automation list.
13. **A kit defines its modulation completely.** `applyKit` sets every depth, kits carry all
    nine in ModTarget order, and only non-zero depths are serialised.
14. **`FactoryKits.inc` is generated.** Edit `kits/*.dfkit` and re-run
    `scripts/emit_factory_kits.py`; never hand-edit the include.
15. **The note map is exactly one of the three, never a union**, and is both a session parameter
    and a global default preference.

### UI and build

16. **A lane shown for the first time must be refreshed before it is laid out**, because its
    type decides which controls are visible. `setMode` does this; without it the sequencer
    page showed every lane as "Off" with its editors missing.
17. **Note captions refresh on the editor's timer, never in a constructor** — the map can change
    while the editor is open. Switching page must not disturb them either: the note follows the
    map, not the page.
17. **The kit parameter is polled rather than attached**, so restoring a session does not
    re-apply the preset over the restored values.
18. **Anything constructing a processor must call `setSettingsDirectoryOverride` first**, before
    construction: the note map's default is read in the constructor.
19. **The macOS re-sign `POST_BUILD` must stay registered after `juce_add_plugin`**, and its
    `codesign --verify` must not be dropped.
20. **`sanitiseFileName` strips trailing dots and spaces**, which are legal on macOS but not on
    Windows.

---

## Deliberately not done

- **No samples.** Every sound is synthesised. This is also why the reference recordings are
  used only as statistical targets and never shipped.
- **No master compressor or limiter.** See *The master bus*.
- **No user preset for the global note map beyond the preference file.** It is a session
  parameter whose default comes from a global file; a full preset browser for it would be
  disproportionate.
