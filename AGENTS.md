# Working on InfinityDrum

Practical guidance for changing this repository. Written for AI agents, but it applies
equally to people.

---

## Golden rules

### 1. Dependencies must be AGPLv3-compatible

InfinityDrum is released under the **AGPLv3** — see [LICENSE](LICENSE). Everything it links,
bundles or vendors must be compatible with that licence. JUCE is dual-licensed and this
project uses the AGPLv3 arm of it.

Before adding any dependency, confirm its licence is AGPLv3-compatible. If that is not
obvious, **ask rather than assume**: a licence mistake is not something a later commit can
undo, and it affects everyone who ever receives a build.

### 2. Never commit the reference samples

`reference_samples/` holds third-party commercial recordings. They are used **only** as
statistical targets when designing kits — measurements of spectral balance, dynamics, crest
factor. No audio from them is ever imported into the plugin, which contains no samples at
all.

They are gitignored. Keep them that way:

- never `git add` them, and never work around the ignore with `git add -f`
- never attach them to a release or a CI artifact
- never reference them from shipped code

`git add -A` will skip them, but check `git diff --cached --name-only` before committing if
you have been anywhere near that directory. This has already gone wrong once: the blobs
were written into the object store and had to be pruned out.

### 3. Always require testing

A change is not finished until it has been verified on the machine it runs on.

- **New behaviour needs a new check.** Add it to `Tools/RenderTests.cpp` unless it is
  something only a human eye can judge.
- **Run the suite before claiming anything works.** `./build/InfinityDrumTests_artefacts/Release/InfinityDrumTests`
  must pass, and `InfinityDrumHostTest` must pass if you touched anything the plugin wrapper sees.
- **Say which check covers it.** "Verified by X" is worth more than "should work".
- **If it cannot be tested automatically, say so.** The editor's mouse gestures have no
  automated coverage; only the maths behind them does. Do not imply otherwise.
- **Test the path that actually runs in CI.** A clean build and an incremental build are not
  the same thing here — see the code-signing note in [ARCHITECTURE.md](ARCHITECTURE.md), where
  an incremental build hid a broken signature for several releases.

---

## Workspace map

### Source

| Path | What lives there |
|---|---|
| `Source/Core/` | Everything that is not drawing: parameter layout, the processor, the engines, kit data, persistence |
| `Source/Core/Models/` | One file per synthesis engine, plus `ModelFactory.h` holding the engine table |
| `Source/UI/` | The editor and its custom widgets |
| `Source/PluginEntry.cpp` | `createPluginFilter()`, the plugin's entry point |

### Tools — offline C++ programs, none of them shipped

| Path | What it does |
|---|---|
| `Tools/RenderTests.cpp` | The main verification suite. Runs headless |
| `Tools/HostLoadTest.cpp` | Loads the built `.vst3` through JUCE's real VST3 hosting code, as a DAW would |
| `Tools/RenderScreenshot.cpp` | Builds the editor and renders it to a PNG |
| `Tools/RenderDemo.cpp` | Writes WAVs you can listen to |
| `Tools/KitLab.cpp` | The kit design harness: exports and renders `.dfkit` files |

### Scripts — Python for kit work, plus the installer

| Path | What it does |
|---|---|
| `scripts/kitgen.py` | Holds the archetypes for twenty of the kits and regenerates `kits/*.dfkit` |
| `scripts/emit_factory_kits.py` | Compiles `kits/*.dfkit` into `Source/Core/FactoryKits.inc` |
| `scripts/kitmetrics.py` | Measures rendered kits against targets taken from the reference recordings |
| `scripts/kitmontage.py` | Joins renders into one montage for listening |
| `scripts/install-macos.sh` | Shipped inside the macOS package; installs the plugin and clears quarantine |

### Content and documentation

| Path | What lives there |
|---|---|
| `kits/` | The kit definitions, as editable `.dfkit` XML. **The source of truth for kit content** |
| `docs/` | The UI screenshot used in the README, `infinitydrum-ui.png` |
| `.github/workflows/` | CI: builds both platforms, tests, packages, releases |
| `README.md` | For users: installing, using, licensing, supporting |
| `ARCHITECTURE.md` | For developers: how it works and why it is built this way |
| `CMakeLists.txt` | The whole build definition |

### Generated or external — all gitignored

| Path | What it is |
|---|---|
| `build/` | Build output. Delete it freely; a clean build is the honest test |
| `renders/` | WAVs written by `InfinityDrumRender` and the kit tools |
| `reference_samples/` | Third-party recordings. Read golden rule 2 |
| `JUCE/` | The framework, cloned separately. Not vendored |

---

## Tasks

**Work is tracked in this repository and nowhere else: <https://github.com/fferri/InfinityDrum>**

Not in a chat transcript, not in a scratch file outside the repo, not only in a commit
message. If a piece of work matters, it belongs in the repository's issue tracker where it
can be found again.

Everything goes in as an item:

- **new work** — a feature or change worth doing
- **bugs found while testing** — including ones you are not fixing now
- **follow-ups a change leaves open** — the thing you deliberately did not do, so the next
  person knows it was a decision rather than an oversight

Each item is written in **English** and carries a **Status** and a **Priority**. For example:

```
Title: Bump the GitHub Actions to their current majors

Status: Open
Priority: P2

actions/checkout and actions/upload-artifact are several majors behind. They still
run - GitHub forces them onto Node 24 - so the deprecation warning is cosmetic.
Do it when the pipeline is otherwise green, on its own, so a failure has one
possible cause.
```

### Priority

| | Meaning |
|---|---|
| **P0** | Broken for users. Crashes, corrupted state, a release that will not install or load. Drop everything |
| **P1** | Wrong or missing behaviour a user would notice. The next thing to pick up |
| **P2** | Polish, refactors, documentation, nice-to-haves |

### Status

`Open` · `In progress` · `Blocked` · `Done`

Use `Blocked` when progress depends on something outside the repository, and say what.

---

## Building

Requirements: CMake 3.22+, a C++20 compiler, and JUCE checked out as a subdirectory (it is
not vendored):

```sh
git clone --depth 1 --branch 9.0.3 https://github.com/juce-framework/JUCE.git JUCE
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

For a universal macOS binary add `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`.

The VST3 bundle lands at `build/InfinityDrum_artefacts/Release/VST3/InfinityDrum.vst3`.

---

## Verifying

```sh
cmake --build build

# The main suite: 491 checks. Drives the processor headlessly.
./build/InfinityDrumTests_artefacts/Release/InfinityDrumTests

# End-to-end through the real VST3 hosting code: 23 checks.
./build/InfinityDrumHostTest_artefacts/Release/InfinityDrumHostTest.app/Contents/MacOS/InfinityDrumHostTest

# Renders the editor to a PNG. The only way to check that the band and dot actually draw.
./build/InfinityDrumScreenshot_artefacts/Release/InfinityDrumScreenshot docs/infinitydrum-ui.png

# Writes WAVs you can listen to, one per engine and one per factory kit.
./build/InfinityDrumRender_artefacts/Release/InfinityDrumRender renders
```

All of them exit non-zero on failure and are safe to run in CI. What each one covers is
described in [ARCHITECTURE.md](ARCHITECTURE.md#testing).

**The offline tools pin their settings directory before constructing a processor.** They
build their note events from General MIDI numbers, so picking up a user's BeatStep Pro
preference would silently ignore most of them and make the output depend on the machine
they ran on. Anything new that constructs a processor should do the same.

---

## Releasing

`.github/workflows/build.yml` builds both platforms on every push to `main`/`master`, runs
the test suite, and attaches the packages to the run:

| Platform | Artifact |
|---|---|
| macOS | `InfinityDrum-macOS-universal.zip` — VST3 and AU, Intel and Apple Silicon in one binary, plus the install script |
| Windows | `InfinityDrum-Windows-x64.zip` |

Pushing a tag starting with `v` turns a build into a release, with both packages attached
and notes generated from the commits:

```sh
git tag -a v1.0.3 -F /path/to/message.txt
git push origin v1.0.3
```

Notes for whoever cuts a release:

- **Wait for the branch run to be green first.** A tag starts its own run, and a tag whose
  build fails leaves a release that did not happen.
- **Never re-point a published tag.** GitHub reads the workflow from the tagged commit, so
  fixing CI *after* tagging means cutting a new patch version — re-pointing rewrites
  something people may already have downloaded. (This was done once for v1.0.0, before it
  had published anything, which is the only time it is acceptable.)
- **A documentation-only push rebuilds nothing.** The workflow ignores `**.md`, `LICENSE`
  and `.gitignore`. Path filters are not evaluated for tag pushes, so tagging always builds.
- **The macOS build is ad-hoc signed, not notarised.** See the README for what users have to
  do, and [ARCHITECTURE.md](ARCHITECTURE.md) for why self-signing does not help.
