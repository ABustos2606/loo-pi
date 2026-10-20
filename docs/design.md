# Loo-pi design

Loo-pi is a multi-loop looper for Peepo, a pedal built on a Raspberry Pi 5 running the Ingen modular host. It is an LV2 plugin made from SooperLooper. Peepo shows it on a page of its own, with every feature of Beebo's Loopler ([peepo-page.md](peepo-page.md)). It must also work as a normal LV2 plugin in Carla or Ardour, without that page.

**Written:** 18 October 2026. **Updated:** 18 October 2026: several loops, a custom page in Peepo. Peepo's `docs/30-beebo-manual-gaps.md` (section 2) records why Peepo needs a looper: Beebo has one (the Loopler, based on SooperLooper and the Echoplex), Peepo has none, and Beebo's loop modules have no public source. The Loopler's features are in Part II of Peepo's `reference/Beebo manual.pdf` (pages 15 to 18) and in `C:\Pedal PI\docs\images\poly-loopler.png`.

## Why SooperLooper

- Beebo's Loopler is SooperLooper. Its commands, special endings, long presses and sync are SooperLooper's, so starting from the same code gives the same behaviour.
- SooperLooper is in two layers:
    - **the core** (`src/plugin.cc`, a former LADSPA plugin): one loop, one channel, per sample, plain C data, no locks;
    - **the application** (`src/engine.cpp`, `src/looper.cpp`): several loops, the selected loop, solo, sync between loops, press and release of commands, long presses, input gain, plus JACK, OSC, MIDI, session files, RubberBand and libsamplerate.
- Loo-pi keeps the core unchanged and **ports the multi-loop parts of the application** into a small real-time layer of its own (see [Several loops](#several-loops)). The application itself is not used: its `Engine` always starts an OSC server thread, adds loops from a non-real-time thread, and pulls in liblo, libxml2, libsigc++, libsndfile, RubberBand and libsamplerate. None of that belongs inside an Ingen block.

## Where it lives

Loo-pi has its own repository, a fork of SooperLooper (https://github.com/ABustos2606/loo-pi), outside Peepo:

- Peepo builds every plugin from another repository at a pinned commit. Loo-pi is built the same way.
- It is a normal LV2 bundle, so it can be tested on a desktop without Ingen or the Pi.
- Its build is a plain Makefile that builds only the LV2 bundle. Upstream's autotools build stays, unused, so upstream fixes can still be merged.
- Its license is **GPL-2.0-or-later** (decided 18 October 2026), inherited from SooperLooper. Peepo (GPL-3.0-or-later) can build and ship it.
- Tagged releases let Peepo update to a new version on purpose.

What stays in Peepo:

- **the Loo-pi page** (LVGL, in Peepo's own style), specified here in [peepo-page.md](peepo-page.md);
- its entry in `config/catalogue/overlay.json`;
- the build step that fetches and builds it at a pinned commit;
- stomp bindings, patches that use it, xrun measurements and test notes.

The plugin URI is `https://github.com/ABustos2606/loo-pi#`, not Peepo's namespace and not SooperLooper's. Fix it before the first saved patch, because changing it later breaks patches.

## One plugin, several loops

- **One plugin, Loo-pi:** stereo in, stereo out, **up to 8 loops** (**tune**: Beebo's screen shows at least five). All loops hear the same input and are mixed to the same output, like Beebo's `loop common in` / `loop common out`.
- Each loop is stereo: two copies of the core, linked the way SooperLooper's `Looper` links channels (`src/looper.cpp:255`): channel 1 takes its sync from channel 0, so both start, stop and wrap on the same sample.
- **The dry signal passes through Loo-pi** (Dry level). Beebo's routing (effects only on the dry path, only on the loop send, only on the loop return) is made with ordinary cables: put effects before Loo-pi for the send, after it with Dry level 0 for the return, and a parallel cable for the dry path. No separate in and out modules are needed.
- **Loops** sets how many loops exist (1 to 8, default 1). Each loop's memory is allocated when the loop is added, off the audio thread (see [Memory](#memory)).
- **The selected loop.** One loop is selected at a time. Commands act on the selected loop, all loops or a given loop, as on Beebo. Next loop and Previous loop change the selection.

## Commands

All of SooperLooper's commands, as the Beebo manual lists them, with SooperLooper's special endings:

| Command | What it does |
| --- | --- |
| Record | Starts a new loop; again to close it and play. With a threshold, recording waits for the input to cross it. Ending with Overdub, Multiply, Insert or Once ends the recording and starts that command |
| Overdub | Plays the loop and adds the input; Feedback sets how much of the old loop stays each cycle |
| Multiply | Repeats the loop while adding to it, making it a whole number of cycles longer |
| Insert | Inserts new cycles into the loop |
| Replace | Replaces the loop with the input; the old loop is not heard meanwhile |
| Substitute | Like Replace, but the old loop is heard while it is replaced |
| Reverse | Plays the loop backwards, also while overdubbing |
| Undo / Redo | Steps through earlier versions; Undo also cancels a command waiting for sync |
| Mute | Silences the loop; it keeps running in time |
| Trigger | Restarts the loop from its start |
| Once | Plays the loop once from its start, then mutes |
| Pause | Stops the loop where it is; again to go on |
| Solo | Mutes every other loop; again to go back |
| Delay | Turns the loop into a delay whose time is the time between the last two presses |
| Rec/Dub | The one-switch command: records an empty loop, overdubs a full one |
| Clear | Erases the loop (Undo all) |
| Next loop / Previous loop | Change the selected loop |

**Press and release.** A command has a press and a release, as in SooperLooper (`Looper::do_event`, `src/looper.cpp:816`). The press acts. A release after a **long press** (more than 1 s, SooperLooper's value) ends the command, unless the loop is already just playing, so holding Overdub overdubs only while held. A long press of Undo is Undo all, of Redo is Redo all, of Rec/Dub is Undo all. Two presses of Rec/Dub within the double-tap time undo twice. Loo-pi keeps these rules exactly, so SooperLooper's and the EDP's manuals apply.

**Where commands come from.** Three ways, all with press and release:

1. **Peepo's Loo-pi page and its stomp bindings** send commands as events on Loo-pi's **Commands** input (an LV2 atom port). Each event names the command, the target (selected, all, or a loop number) and press or release. Events are queued by Ingen, so two stomps pressed in the same block are both seen. See [peepo-page.md](peepo-page.md).
2. **Command controls** for the most used commands on the selected loop (Rec/Dub, Overdub, Undo, Mute, Trigger, Pause, Next loop), as ordinary control ports. 0 to 1 is a press, 1 to 0 a release. They let any host, a Foot switch module's cable or Peepo's generic stomp assignment (Momentary) drive Loo-pi.
3. **MIDI** on the same Commands input, later (Beebo binds commands to MIDI CC and notes). Not in the first milestones.

## Settings

SooperLooper keeps most settings per loop. Loo-pi follows Beebo's split.

**Per loop** (Beebo's LEVEL / SYNC and RATE sections, for the selected loop):

- Loop level (SooperLooper's Wet), Input gain, Rec threshold, Feedback;
- Sync (record and stop to the sync source), Play sync (start playback on a sync boundary), Play feedback (Feedback also applies while playing);
- Rate (varispeed, 0.25x to 4x; pitch moves with speed).

**Global** (Beebo's global settings):

- Dry level, Loops, selected loop;
- Quantize (Off, Cycle, 8th, Loop), Round, Mute quantized, Overdub quantized, Replace quantized;
- Sync to (None, Tempo, Loop 1 to 8), Relative sync, 8ths per cycle, BPM;
- Fade (crossfade at loop edges), Safety feedback;
- Loop memory (seconds per newly added loop).

Every setting is a control port, so patches save it.

## Tempo and sync

Peepo gives every patch a tempo (30 to 300 BPM) on a BPM control port of each module (Peepo's `docs/29-parameters-files-tempo.md`).

- **Sync to Tempo:** pulses every eighth note from BPM, as SooperLooper's `Engine::generate_sync` makes them (`src/engine.cpp`). 8ths per cycle sets the cycle (default 16: two bars of 4/4).
- **Sync to Loop N:** the other loops sync to loop N's cycle, SooperLooper's usual way of keeping several loops in time.
- **Sync to None:** free loops.
- Peepo has no MIDI clock or JACK transport for modules, so SooperLooper's MIDI and JACK sync sources are left out. MIDI clock is a later question.
- **Clock outputs** (CV): a trigger at each cycle start and at each loop start of the sync loop, so other modules (Discipline's Size, clock dividers, sequencers) can follow the loops.
- The loop can't set the patch tempo (BPM is an input). Tap tempo on a stomp sets the patch tempo before recording.

## The Peepo page

Peepo shows Loo-pi on a page of its own instead of the generic bars, as it does for the Tuner and the Euclidean sequencer: a row of loop cards (state, cycle and total time, position), the commands, the per-loop and global settings, and stomp binding. It is written in Peepo's style, not Beebo's. The full specification is [peepo-page.md](peepo-page.md).

What Loo-pi gives the page:

- **Per-loop outputs:** state, next state, waiting, loop length, cycle length, position. Peepo watches them while the page is open (Ingen sends monitored outputs at 25 Hz, Peepo engine patch 0006).
- **Global outputs:** the selected loop, free memory, the soloed loop.
- **The Commands input**, for every command with its target, press and release.
- **Control ports** for every setting, set the usual way.

## Memory

- SooperLooper allocates each core's memory in `instantiate`, rounded up to a power of two. 80 s becomes 87 s at 48 kHz: 16 MB per channel, **32 MB per stereo loop**. The undo history shares that memory.
- **Eight loops at 80 s would be 256 MB.** So loops are allocated when they are added, not all at once, and Loop memory sets the length of a newly added loop (default 40 s, 16 MB stereo, **tune**).
- Adding a loop allocates and touches its memory in an **LV2 worker** (Ingen supports `work:schedule`, `src/server/LV2Block.cpp:473`), never in `run()`. The loop appears when the worker answers. Removing a loop frees it the same way.
- The core's `getenv("SL_SAMPLE_TIME")` is replaced with a length passed in by Loo-pi.
- **Loops are lost when a patch reloads** in the first milestones, as with Discipline and Clouds. Saving them is a later question.

## Bypass

Loo-pi has an `lv2:enabled` port. Peepo's overlay marks it `"bypass": "port"`, so bypass sets the port to 0 and the block keeps running (Ingen's own bypass never calls `run()`). Bypassed, Loo-pi passes the dry signal at Dry level and mutes all loops, which keep their place and their contents; turning it back on unmutes them in time. Milestone 1 implements this with a 10 ms fade and preserves a loop that was already muted before bypass.

## What changes in the core

The core is kept; only what doesn't fit Ingen changes:

- `getenv` in `instantiate`: replaced with a length from Loo-pi.
- Loop memory from `calloc`, pages untouched: touched by the worker that allocates it.
- Latency and application ports (`InputLatency`, `OutputLatency`, `TriggerLatency`, `SyncOffsetSamples`): fixed by Loo-pi, not exposed.

Changes are kept small and commented, so upstream merges stay possible.

## Decisions

Settled with the user on 19 October 2026:

- **License:** GPL-2.0-or-later.
- **At most 8 loops.**
- **Rate is plain speed only** (varispeed: pitch follows speed). No time stretch or pitch shift, so no RubberBand.
- **Saving loop audio with the patch waits** for milestone 4.
- **The Peepo page layout** is the one in [peepo-page.md](peepo-page.md), chosen by us; the user did not supply a mockup.

## Open questions

| Question | Why it matters | When to settle |
| --- | --- | --- |
| Saving loops | Loops are lost on reload. LV2 state could write each loop to a WAV file beside the patch, outside `run()` | Milestone 4 |
| Separate loop outputs | Beebo plans "individual loop in and outs"; SooperLooper has them | Milestone 4 |
| MIDI binding and MIDI clock | Beebo binds commands to MIDI and syncs to MIDI clock; Peepo has no MIDI yet | When Peepo has MIDI |
| Telemetry load | 8 loops x 6 outputs at 25 Hz is 1,200 values a second to the interface | Measured 20 October 2026: 1,275 values/s, sent unchanged or not; raises Ingen and interface CPU and the DSP peak ([evidence](milestone-2-validation.md#on-the-pi-20-october-2026)). Peepo should subscribe only to existing loops |

## Milestones

- [x] Fork SooperLooper as Loo-pi. License GPL-2.0-or-later.
- [ ] Milestone 0: the plain Makefile, the LV2 glue and `.ttl` with the full port list, building only the core. Loo-pi loads in jalv and in Ingen under WSL. Implemented; desktop validation passes, physical Pi and release checks remain ([evidence](milestone-0-validation.md)).
- [ ] Milestone 1: one stereo loop through the multi-loop layer: Record, Overdub, Rec/Dub, Mute, Undo, Redo, Clear, Trigger, press, release and long press, levels, feedback, threshold, bypass, the real-time fixes. Implemented; desktop and emulated arm64 checks pass, physical Pi and release checks remain ([evidence](milestone-1-validation.md)).
- [ ] Milestone 2: several loops: Loops and the worker, selected loop and targets, Solo, Next and Previous, the Commands input, the per-loop outputs. Peepo's Loo-pi page, first version. Implemented; physical Pi/xrun, telemetry and release gates remain ([evidence](milestone-2-validation.md)).
- [ ] Milestone 3: the rest of the commands (Multiply, Insert, Replace, Substitute, Reverse, Once, Pause, Delay), Quantize, sync to Tempo and to a loop, Rate, the clock outputs. The page complete, with stomp binding. Implemented in Loo-pi and in Peepo's page; Pi listening and release remain ([evidence](milestone-3-validation.md)).
- [ ] Milestone 4: saving loops, then decide separate loop outputs and MIDI.
