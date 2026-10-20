# Loo-pi

Loo-pi is a multi-loop LV2 looper for **Peepo**, made from SooperLooper (a fork of https://github.com/essej/sooperlooper), with every feature of Beebo's Loopler. It keeps SooperLooper's looping core (`src/plugin.cc`), ports the real-time parts of its multi-loop application (`src/looper.cpp`, `src/engine.cpp`) into `lv2/`, and replaces the JACK application with an LV2 plugin. Peepo is the host that matters, and shows Loo-pi on a page of its own. Read `docs/design.md` before changing behaviour or ports: it is the spec. `docs/implementation-plan.md` says how to build it; `docs/peepo-page.md` is the contract with Peepo's Loo-pi page.

## Peepo

Peepo is a guitar pedal built on a Raspberry Pi 5 (2 GB, Debian 12 arm64) running the **Ingen** modular host on JACK, with an LVGL touch UI. Users patch LV2 modules together, including CV modules (LFOs, envelopes, random sources). It has two encoders and three momentary stomps. It was called "Pedal PI" before; old names (`pedalpi`, `urn:pedalpi:ns#`) still turn up.

- Repository: `C:\Pedal PI` (https://github.com/ABustos2606/Peepo). It is readable from here (see `.claude/settings.json`).
- Docs worth reading there:
    - `docs/30-beebo-manual-gaps.md` (section 2, Looper): what Beebo's looper offers and why Peepo needs one;
    - `reference/Beebo manual.pdf`, Part II (pages 15 to 18), and `docs/images/poly-loopler.png`: the Loopler's commands and screen, the feature list Loo-pi must match;
    - `docs/03-interface-and-controls.md` (Visual style) and `docs/25-effects-and-tuner.md`: Peepo's look, and how a module gets a page of its own (Tuner, Euclidean);
    - `docs/18-physical-controls.md`: what a stomp can do to a control (toggle, momentary, Foot switch module);
    - `docs/22-plugin-catalogue.md` and `config/catalogue/overlay.json`: how plugins are named, grouped and described in the UI;
    - `docs/29-parameters-files-tempo.md`: patch tempo, BPM ports and times in beats;
    - `docs/26-pedal-inspired-plugins.md`: the first plugin written for Peepo (Discipline) and the Peepo-side steps;
    - `reference/upstream/ingen/`: Ingen's source;
    - `reference/upstream/sooperlooper/`: Poly Effects' SooperLooper fork, which Beebo's looper is based on.
- `C:\LV2-own-plugins\discipline` is the sister project: same host limits, same kind of build, tests and milestones. Copy its patterns (LV2 glue, `ttl_check`, in-process test host, timing test) rather than reinventing them.
- Peepo builds every third-party plugin from its own repository at a pinned commit (`tools/ingen-spike/14-more-plugins.sh`, `reference/manifest.json`). Loo-pi will be built the same way, so tag releases and keep the build plain (`make`, no network access, works natively on arm64).

## What is kept and what is not

- **Kept:** the looping core in `src/plugin.cc` and `src/plugin.hpp`, and the small headers it needs (`event.hpp`, `utils.hpp`, `ladspa.h`).
- **Ported, not built:** the real-time parts of `Looper` (stereo channel link, press/release, long press, double tap, input gain) and `Engine` (several loops, selected loop, targets, solo, sync between loops, tempo pulses). Copy their rules exactly, naming the source function; don't reinvent them.
- **Not built:** the JACK driver, OSC, MIDI binding, the wxWidgets GUI, RubberBand, libsamplerate, the panner and the session code. They stay in the tree, untouched, so upstream changes can still be merged. The Loo-pi Makefile ignores autotools and builds only the LV2 bundle.
- New code lives in `lv2/` (glue, multi-loop layer, port enums), `tools/` and `loo-pi.lv2/` (Turtle). Change upstream files only where the plan says, and keep those changes small and commented.
- **The Loo-pi page is Peepo code** (LVGL, in Peepo's own style), not an LV2 UI. Loo-pi gives it per-loop output ports and a `commands` atom input; keep `docs/peepo-page.md` in step with the ports.

## Host limits that shape the code

- **xruns are the limit, not average CPU.** Ingen runs the whole patch as one JACK client. In `run()`: no allocation, no locks, no file or console I/O, no bulk work such as clearing a whole buffer at once. Loop memory (about 16 MB per 40 s stereo loop) is allocated and its pages touched in the LV2 worker when a loop is added, never in `run()`.
- **`run()` arrives in slices.** Ingen splits a period at every control change, so `n_samples` varies. The core is per sample, which is fine; the glue must not assume one `run()` per period.
- **Blocks run on several threads.** Two Loo-pi instances may run at the same moment. Anything shared between instances must be lock-free.
- **Controls arrive once per audio block.** Commands with press and release come as events on the `commands` atom input (Ingen queues them) or as command controls (0 to 1 press, 1 to 0 release); the glue turns each into the core's Multi command value.
- **Ingen's own block bypass never calls `run()`.** A looper bypassed that way stops dead. Loo-pi has an `lv2:enabled` port and Peepo's overlay marks it `"bypass": "port"`.
- **Ingen supports the LV2 worker** (`work:schedule`); that is where loops are allocated and freed.
- **Unpatched inputs read silence.** A plugin can't tell whether a port is connected; don't guess.
- **Loops are not saved with patches** until LV2 state is added (an open question in the spec).
- Everything a user sets is a control port, so patches save it.

## Rules

- License: **GPL-2.0-or-later** (decided 18 October 2026), because SooperLooper is. It cannot be relicensed to MIT. Keep Jesse Chappell's copyright notices. New files start with `SPDX-License-Identifier: GPL-2.0-or-later` and `Copyright (c) 2026 Alejandro Bustos`; the plugin declares `doap:license <https://spdx.org/licenses/GPL-2.0-or-later>`.
- Plugin URI: `https://github.com/ABustos2606/loo-pi#...`. Never use Peepo's namespace or SooperLooper's. Don't change a URI or port symbol once a patch has been saved with it.
- Port names and descriptions appear on Peepo's small screen: short, plain ASCII (its font draws other characters as boxes), and they say what the control does, never who made it.
- Credit SooperLooper and Jesse Chappell in the README and `doap:` metadata, not in port names.
- Line endings are LF. Files go to Linux and the Pi.
- Use Peepo's name, never "Pedal PI", in new text.

## Testing

- Desktop: any LV2 host (Carla, Ardour, `jalv`), and `lv2lint` for the `.ttl` files.
- Peepo's development engine runs under WSL (Debian) with Ingen in `/opt/peepo`; Peepo's `tools/ingen-spike/env.sh` starts it.
- On the Pi: measure xruns with Loo-pi in a typical patch before calling a milestone done.
