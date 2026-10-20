# Loo-pi implementation plan

**Written:** 18 October 2026. **Updated:** 18 October 2026, for several loops and the Peepo page. **Follows:** [design.md](design.md), the spec, and [peepo-page.md](peepo-page.md), the page contract. Where this plan and the spec disagree, the spec wins and this plan is fixed. **Checked against:** SooperLooper's `src/plugin.cc`, `src/plugin.hpp`, `src/event.hpp`, `src/looper.cpp` and `src/engine.cpp` at upstream commit `c5e22ce`; Ingen's `SetPortValue.cpp` and `LV2Block.cpp` in Peepo's `reference/upstream/ingen/`; Discipline's implementation plan, whose Ingen findings apply unchanged.

This plan says how to build Loo-pi: code layout, ports, the multi-loop layer, the worker, real-time fixes, tests and milestones. Values that only playing can settle are given a starting value and marked **tune**.

## 1. What Ingen's source changes

From Discipline's plan (section 1), plus two facts checked for Loo-pi:

1. **Engine bypass does not run the plugin.** Peepo bypasses Loo-pi through its `enabled` port (`"bypass": "port"`).
2. **`run()` is called in slices.** Ingen splits a period at every control change. Edge detection, long-press timers and the tempo counter count samples, not calls.
3. **Blocks run on several threads.** Two Loo-pi instances may run at once; they share nothing.
4. **Block length** comes from `bufsz:nominalBlockLength` / `maxBlockLength` through `options:options`. Loo-pi uses the maximum to size its scratch buffers.
5. **Events on an atom input.** A value set on an `atom:Sequence` input is appended to the port's buffer as an event at its time in the block (`src/server/events/SetPortValue.cpp:114`). So Peepo's page can send commands without losing one when two arrive in the same block.
6. **The LV2 worker is supported.** Ingen provides `work:schedule` to plugins that require it and calls `work()` outside the audio thread (`src/server/LV2Block.cpp:473`). Loop memory is allocated there.

## 2. Technology and build

- **Plain LV2 C API in C++17**, no DPF.
- **License:** GPL-2.0-or-later. **Dependencies:** `lv2-dev` headers only. Nothing from upstream's dependency list (JACK, liblo, libxml2, sigc++, wxWidgets, RubberBand, libsamplerate, libsndfile) is linked.
- **One binary, `loo-pi.so`**, in `loo-pi.lv2/` beside `manifest.ttl` and `loo-pi.ttl`.
- **Makefile** at the top of the repository (upstream's `.gitignore` ignores `Makefile`; Loo-pi's is excepted). It builds `src/plugin.cc` and `lv2/` only:
    - `make` builds `build/loo-pi.lv2/`;
    - `make install` uses `DESTDIR`, `PREFIX` (default `/usr/local`) and `LV2DIR` (default `$(PREFIX)/lib/lv2`);
    - `make test`, `make lint`, `OPT` and `SAN=1` as in Discipline.
- **Flags:** `-std=c++17 -fPIC -fvisibility=hidden -Wall -Wextra -fno-math-errno -fno-trapping-math`, not `-ffast-math`. Only `lv2_descriptor` is exported. The core's old warnings are silenced for `plugin.cc` only, not fixed wholesale.
- **Releases** are git tags (`v0.1.0` ...). Upstream's tags (`1.7.x`) stay; Loo-pi's start with `v`.

### Repository layout (new files only)

```
Makefile                  the LV2 build; upstream's autotools files untouched
README.md                 Loo-pi: what it is, build, install, credit to SooperLooper
README.sooperlooper.md    upstream's README, kept
loo-pi.lv2/
    manifest.ttl
    loo-pi.ttl            all ports (generated, section 3)
lv2/
    lv2_entry.cpp         lv2_descriptor(), instantiate, run, cleanup, worker interface
    ports.hpp             port indices, the single source in C++
    stereo_loop.hpp/.cpp  one stereo loop: two cores, press/release, input gain (from Looper)
    loop_set.hpp/.cpp     up to 8 loops: selection, targets, solo, sync routing, mix (from Engine)
    commands.hpp          atom events and control edges to (target, command, press)
    tempo_sync.hpp        eighth-note pulses from BPM (from Engine::generate_sync)
    loop_memory.hpp/.cpp  worker side: allocate, touch, lock and free loop memory
    loop_state.hpp/.cpp   LV2 state: loops to WAV files and back (4.7)
tools/
    gen_ttl.py            writes loo-pi.ttl from one table (the per-loop ports repeat 8 times)
    render.cpp            WAV in, a command script, WAV out, for listening
tests/
    host.hpp/.cpp         in-process LV2 host with random slicing and a worker (from Discipline)
    test_*.cpp
    ttl_check.py          .ttl indices and symbols against ports.hpp
docs/
    design.md, implementation-plan.md, peepo-page.md
```

`src/` keeps upstream's files. Only `plugin.cc` is compiled, with the changes in section 7. The code ported from `looper.cpp` and `engine.cpp` is copied into `lv2/` with a comment naming the function it came from, keeping Jesse Chappell's copyright line beside Loo-pi's.

## 3. Ports

Port **symbols and URIs are permanent** once a patch has been saved with them. All ports are declared in milestone 0, including those whose behaviour comes later; until a port does something it is inert. Later ports are only appended, with `lv2:minorVersion` raised. Names are plain ASCII, at most 16 characters. `kMaxLoops = 8` (decided 19 October 2026).

### Loo-pi: `https://github.com/ABustos2606/loo-pi#loopi`

**Audio and events**

| # | Symbol | Name | Kind | Notes |
|---|---|---|---|---|
| 0 | `in_l` | In L | audio in | |
| 1 | `in_r` | In R | audio in | |
| 2 | `out_l` | Out L | audio out | |
| 3 | `out_r` | Out R | audio out | |
| 4 | `commands` | Commands | atom in, `atom:Sequence`, supports `atom:Int` (later `midi:MidiEvent`) | the page's command events ([peepo-page.md](peepo-page.md#the-contract)) |

**Command controls**, acting on the selected loop. 0 to 1 is a press, 1 to 0 a release.

| # | Symbol | Name | Command |
|---|---|---|---|
| 5 | `rec_dub` | Rec/Dub | `RECORD_OR_OVERDUB` |
| 6 | `overdub` | Overdub | `OVERDUB` |
| 7 | `undo` | Undo | `UNDO` (long: Undo all) |
| 8 | `mute` | Mute | `MUTE` |
| 9 | `trigger` | Play | `TRIGGER` |
| 10 | `pause` | Pause | `PAUSE` |
| 11 | `next_loop` | Next loop | select next |

All are `lv2:toggled` with `pprops:trigger`, default 0.

**Global settings**

| # | Symbol | Name | Kind | Range | Default | Notes |
|---|---|---|---|---|---|---|
| 12 | `dry_level` | Dry level | control | 0 - 1 | 1 | |
| 13 | `loops` | Loops | integer | 1 - 8 | 1 | adding allocates through the worker |
| 14 | `selected` | Selected loop | integer | 1 - 8 | 1 | clamped to `loops` |
| 15 | `quantize` | Quantize | enum | 0 Off, 1 Cycle, 2 8th, 3 Loop | 0 | |
| 16 | `round` | Round | toggle | | 0 | |
| 17 | `mute_quant` | Mute quantized | toggle | | 0 | |
| 18 | `dub_quant` | Dub quantized | toggle | | 0 | |
| 19 | `repl_quant` | Repl quantized | toggle | | 0 | |
| 20 | `sync_to` | Sync to | enum | 0 None, 1 Tempo, 2 - 9 Loop 1 - 8 | 0 | |
| 21 | `rel_sync` | Relative sync | toggle | | 0 | |
| 22 | `eighths` | 8ths/cycle | integer | 1 - 128 | 16 | |
| 23 | `bpm` | BPM | control | 30 - 300 | 120 | Peepo's patch tempo |
| 24 | `fade` | Fade ms | control | 0 - 50 | 1.3 | the core's FadeSamples; SooperLooper's engine passes 64 samples |
| 25 | `safety_fb` | Safe feedback | toggle | | 1 | |
| 26 | `loop_secs` | Loop memory | control, s | 10 - 300 | 40 | for loops added from now on |
| 27 | `enabled` | On | toggle, `lv2:designation lv2:enabled` | | 1 | bypass; hidden in Peepo |

**Global outputs**

| # | Symbol | Name | Kind | Notes |
|---|---|---|---|---|
| 28 | `selected_out` | Selected | control out | 1 - 8, after clamping and Next/Previous |
| 29 | `soloed` | Soloed | control out | 0 none, else the loop |
| 30 | `free_mem` | Free memory | control out, s | of the selected loop |
| 31 | `cycle_clock` | Cycle clock | CV out | 1.0 for 1 ms at each cycle start of the sync loop |
| 32 | `loop_clock` | Loop clock | CV out | 1.0 for 1 ms at each loop start of the sync loop |

**Per loop**, for N = 1 to 8, starting at index `33 + (N - 1) * 14`:

| Offset | Symbol | Name | Kind | Range | Default | Core port |
|---|---|---|---|---|---|---|
| 0 | `lN_level` | LN Level | control | 0 - 1 | 1 | WetLevel |
| 1 | `lN_in_gain` | LN Input gain | control | 0 - 1 | 1 | (Looper's input gain) |
| 2 | `lN_thresh` | LN Threshold | control | 0 - 1 | 0 | TriggerThreshold |
| 3 | `lN_feedback` | LN Feedback | control | 0 - 1 | 1 | Feedback |
| 4 | `lN_sync` | LN Sync | toggle | | 0 | Sync |
| 5 | `lN_play_sync` | LN Play sync | toggle | | 0 | PlaybackSync |
| 6 | `lN_play_fb` | LN Play feedbk | toggle | | 0 | UseFeedbackPlay |
| 7 | `lN_rate` | LN Rate | control, log | 0.25 - 4 | 1 | Rate (UseRate on when not 1) |
| 8 | `lN_state` | LN State | control out | -2 - 20 | | State; -2 no memory yet |
| 9 | `lN_next` | LN Next state | control out | | | NextState |
| 10 | `lN_waiting` | LN Waiting | control out | 0/1 | | Waiting |
| 11 | `lN_len` | LN Length | control out, s | | | LoopLength |
| 12 | `lN_cycle` | LN Cycle | control out, s | | | CycleLength |
| 13 | `lN_pos` | LN Position | control out, s | | | LoopPosition |

145 ports in all. `tools/gen_ttl.py` writes them from one table, so the eight copies can't drift; `ttl_check` compares the result with `ports.hpp`. Enums use `lv2:integer`, `lv2:enumeration` and `lv2:scalePoint`. The class is `lv2:DelayPlugin` (LV2 has no looper class; Peepo's overlay sets the category). Loo-pi requires `urid:map` and `work:schedule`, provides the worker and state interfaces, uses `state:mapPath`, `state:makePath` and `state:freePath` when offered, and is `lv2:hardRTCapable`. Milestone 4 adds no ports; `lv2:minorVersion` is 4.

The core's ports that are not exposed get the values SooperLooper's engine passes (`src/looper.cpp:200`): `ScratchPosition` 0, `TapDelayTrigger` 0, `RedoTap` 0, latencies and `SyncOffsetSamples` 0, `RoundIntegerTempo` 0, `TempoInput` = BPM. The application's pan, discrete loop ports, RubberBand and resampling are not ported.

## 4. The multi-loop layer

SooperLooper's `Looper` and `Engine` classes do this in the application. Loo-pi ports only their real-time parts, keeping their rules, and leaves out everything that needs OSC, files, threads or allocation.

### 4.1 A stereo loop (`stereo_loop`)

Ported from `Looper` (`src/looper.cpp`):

- Two core instances, one per channel. Channel 0 reads the sync buffer chosen by the loop set (4.3) and writes its own sync output; channel 1 reads channel 0's sync output, with `Sync` fixed at 1 and its state outputs going to a dummy (`Looper::initialize`, line 255).
- **Press and release** (`Looper::do_event`, line 816): `_down_stamps`, long press at 1 s, double tap for Rec/Dub, the long-press translations (Undo to Undo all, Redo to Redo all, Rec/Dub to Undo all) and the "not if already playing" rule. Copied, not rewritten.
- Milestone 1 adapters retain the specified rules at sample zero and across releases using separate held/last-press stamps. The upstream zero sentinel otherwise loses those gestures, and its core aliases `UNDO_TWICE` to one Undo. Loo-pi performs the second Undo after the first fade. The core looping algorithm is unchanged.
- **Input gain**, smoothed per sample, before the cores (`Looper::run_loops`).
- Gains and Feedback use fixed 10 ms linear ramps. Feedback ramps in steps of 32 samples counted from the plugin's start, and while it ramps the core runs in chunks that never cross a step, so each chunk sees constant feedback (an LFO on Feedback then costs about the same as a steady value). History fades run the core one sample at a time; this makes output bit-identical under Ingen's variable slicing without rewriting the core's block-dependent ramps/commit points. Loop level is applied to the loop return; Dry level to the unscaled dry input.
- Each control is written to both cores' port arrays; Multi is set for one slice per command and back to -1, as the engine does.
- **Rate** (`Looper::run_loops`, resampled branch): upstream resamples input, sync and output with libsamplerate's `SRC_LINEAR`, which Loo-pi does not build. Loo-pi interpolates linearly itself: the input into loop time at Rate, the output back at 1/Rate, reading about three loop-time frames behind the newest so both neighbours exist. A sync value moves whole to the next loop-time frame instead of being interpolated, so no pulse is lost or split. Rate exactly 1 uses the direct path, unchanged from Milestone 1. Press timing (long press, double tap) counts host frames. Scratch buffers are sized for Rate 4 when the loop is allocated.
- **Quantized Reverse** commits the core's rate on the boundary sample, but the core keeps stepping with its local copy until the run ends, so the direction would flip wherever the host ended the run. While a quantized Reverse is pending the cores run one frame at a time, as during history fades, so the flip always lands on the frame after the boundary.
- A loop with no memory yet reports state -2, passes nothing and ignores commands.

### 4.2 The loop set (`loop_set`)

Ported from `Engine` (`src/engine.cpp`):

- Up to `kMaxLoops` stereo loops, `loops` of them active, `selected` among them.
- **Targets.** A command for "selected" goes to the selected loop, "all" to every active loop, "loop n" to loop n. Next and Previous wrap within the active loops.
- **Solo and exclusive commands** (`Engine::do_global_rt_event`, line 958): Solo, Solo next and previous, and the `RECORD_*_EXCL` / `*_SOLO` variants mute or unmute the other loops exactly as there.
- **Mix.** Every active loop gets the same input (after its own input gain); their outputs are summed with the dry signal at Dry level. No limiter: the user sets levels, as in SooperLooper.

### 4.3 Sync

- **Tempo** (in `loop_set`, from `Engine::generate_sync` and `calculate_tempo_frames`): a counter writes 2.0 into a per-slice buffer allocated in `instantiate`, every `samplerate * 30 / bpm` frames with Quantize 8th or that times 8ths/cycle with Quantize Cycle; with Quantize Off or Loop the buffer is 1.0 everywhere, as upstream. A BPM change restarts the counter (`Engine::set_tempo`; this replaces the earlier "at the next wrap"). The counter also restarts on `activate`. Upstream writes a pulse due on the sample after the block past the buffer's end and so loses it; Loo-pi writes it on the next slice's first sample.
- **Loop N:** loop N runs first in every slice; its channel 0 sync output is the sync buffer for every other loop, as in `Engine::process`. Loop N itself runs with Sync 0 (`Looper::run`).
- **None:** no sync source. Adaptation: upstream gives a loop with Sync on an all-zero buffer, so its recording would wait forever; Loo-pi runs such a loop free. The same applies when Sync to names a loop that does not exist. TempoInput is 0 with None, as upstream.
- **Relative sync** makes the core's Sync 2 for synced loops; channel 1 follows with 2 (`Looper::run`, `_slave_sync_port`).
- **Clock outputs:** a 1 ms trigger (1.0) on the host frame that plays the first frame of each cycle (cycle clock) and of the loop (loop clock) of the sync loop, or of the selected loop with Sync to Tempo or None. Closing a recording into Play starts both. `stereo_loop` tracks them per core run: the core moves its position by exactly one frame per frame, so the frame that played each boundary follows from the positions before and after the run, including one change of direction; the frames map back to host frames through the resampler. Bit-identical under any host slicing.

### 4.4 Commands (`commands.hpp`)

- **Atom events** on `commands`, in time order within the slice: an `atom:Int` decoded as `target * 256 + command * 2 + press` ([peepo-page.md](peepo-page.md#the-contract)). Unknown types and out-of-range values are ignored. The slice is split at each event's time so a command lands on its sample.
- **Command controls** (ports 5 to 11): 0 to 1 is a press, 1 to 0 a release, to the selected loop.
- Both go through the same path into 4.1 and 4.2, so every source has long presses and double taps.

The Milestone 1 subset of the atom input was brought forward so dedicated
Record, Redo and Clear are reachable without adding or changing ports. Targets
0 (selected), 1 (all) and 2 (loop 1) reach the sole loop; targets for absent
loops and later commands are ignored. Multi-loop routing remains Milestone 2.
Coincident command requests retain control-port/event order and are serviced
on successive samples, with the idle sample required between identical Multi
values; they are never postponed by an entire host slice.

### 4.5 Loops and memory (`loop_memory`, the worker)

- When `loops` rises, `run()` schedules one work request per new loop, carrying the loop index and `loop_secs`. It never allocates.
- `work()` (worker thread) calls the core's `instantiate` twice with that length, touches every page, tries `mlock`, and responds with the two handles.
- `work_response()` (audio thread) installs them, connects their ports, and the loop's state leaves -2.
- When `loops` falls, the removed loops are muted over 10 ms, detached in `run()`, and their handles sent to the worker, which calls `cleanup()`.
- `instantiate` allocates loop 1 directly (it is not the audio thread), so a fresh Loo-pi plays at once. `work:schedule` is required (settled in milestone 0); hosts without a worker cannot instantiate Loo-pi. This keeps the same host contract when adding loops arrives in milestone 2.
- A failed allocation leaves the loop at state -2; the page shows "no memory" on its card.

### 4.7 Saving loops (LV2 state)

Ported from `Looper::save_loop` and `Looper::load_loop` (`src/looper.cpp`); libsndfile is not built, so `loop_state.cpp` writes and reads the WAV files itself.

- **What is saved.** `#loops` (`atom:Int`, the loop count) and, for each loop with a closed loop, `#loopN` (`atom:Path` to `loopN.wav`), `#loopN_cycle` (`atom:Long`, cycle frames) and `#loopN_memory` (`atom:Float`, the loop's memory in seconds). Keys are under `https://github.com/ABustos2606/loo-pi#` and are permanent. The file is 32-bit float stereo at the plugin's rate, starting at the loop's sync position, as `sl_read_current_loop_audio` reads it. A loop still recording, in Delay, or empty is not saved. Undo history is not saved (upstream saves only the current loop).
- **Save runs while `run()` does** (LV2 state's own threading class; Ingen saves from its own thread). Each `StereoLoop` publishes its geometry (start, length and sync shift per channel, cycle, state) at the end of `process()` through a sequence lock. `capture()` reads it, copies the loop out of the core's memory, and checks that the geometry did not change meanwhile; it retries up to four times and keeps the last copy. Samples an Overdub writes during the copy may be old or new. No change in one `process()` call can overwrite the current loop's memory: a new recording starts after it in the ring.
- **No loop is freed while it is read.** `LoopSet` shares a pointer per loop, cleared when a loop starts retiring. The save sets `saving_`, then reads the pointers; the audio thread clears a pointer, and frees the loop through the worker only when `saving_` is false (both sequentially consistent).
- **Without `makePath` or `mapPath`** nothing is saved. Ingen's block duplicate is such a case; the copy starts with empty loops.
- **Restore** is in the instantiation class; Ingen calls it inside `instantiate`, before the block runs. For each saved loop it allocates a new `StereoLoop` with the saved memory (or more, if the file is longer) and runs `load()`: upstream's sequence of no threshold, sync, fade or quantize; Mute, Record, the frames, then Pause, because the loop it loads into is empty. Adaptation: upstream loads every file as one cycle; Loo-pi sets the saved cycle when it divides the loop, so a multiplied loop and the loops synced to its cycle keep their timing. Missing or unreadable files leave the loop empty. Another sample rate is not converted.
- **The Loops control arrives after the state.** Ingen restores state at instantiation and sets the saved control values later, so the first blocks may run with `loops` at its default of 1, which would retire the restored loops. `LoopSet::hold()` keeps the saved count until the `loops` input has reached it or has changed from the first value seen.
- **A restore while the worker allocates** (a host that restores a running instance) keeps the restored loop; the late allocation is sent back to the worker to be freed.

### 4.6 Bypass

`enabled` 1 to 0: every active loop gets `MUTE_ON` with a 10 ms fade; the dry signal keeps Dry level. 0 to 1: `MUTE_OFF` for the loops that were not muted before. Loops keep running in time.

## 5. Real-time rules, checked

- `run()`, `work_response()` and everything they call: no allocation, locks, I/O, `getenv`, `std::string`, iostream.
- The core is compiled with `LOOPDEBUG` off, so its `DBG(fprintf...)` lines compile to nothing.
- No clear over loop memory in `run()`. Clear is the core's `UNDO_ALL`, which only moves list pointers.
- Finite check per slice on the outputs: a non-finite sample writes silence for the slice and sends `UNDO_ALL` to the loops whose output was not finite.
- The worker and the audio thread share only the LV2 worker's message passing. The state save thread shares the sequence-locked geometry, the shared loop pointers and `saving_` (4.7).

## 6. Telemetry

The per-loop outputs are written once per slice from channel 0's core outputs. Peepo watches them only while its page is open. 8 loops x 6 outputs at 25 Hz is 1,200 values a second; measure it on the Pi in milestone 2. If it is too much, Peepo watches `lN_pos` only for the loops on screen, or Loo-pi adds an atom output that sends one compact message per loop when something changes (appended port).

Measured on the Pi on 20 October 2026: Ingen sends all 51 subscribed outputs at 25 Hz whether they change or not (1,275 values, 203 KiB a second). With the page open Ingen's CPU went from 6.4 % to 15.5 % of a core, the interface's from 0.4 % to 18.4 %, the DSP peak from 5 % to 29 %, and xruns appeared to rise. First step: Peepo subscribes only to the existing loops (27 outputs with 4). See [milestone-2-validation.md](milestone-2-validation.md#on-the-pi-20-october-2026).

## 7. Changes to upstream files

Each change is marked `// Loo-pi:` in the source.

1. `plugin.cc`: drop `getenv("SL_SAMPLE_TIME")`; take the length as an explicit argument to `sl_instantiate(sample_rate, sample_seconds)`. The retained LADSPA entry point calls it with `SAMPLE_MEMORY`. Implemented in milestone 0: a shared setter would race when two instances or workers allocate at once.
2. `plugin.cc`: remove `#include <iostream>` and guard `cerr` uses behind `LOOPDEBUG` if any remain outside `DBG`.
3. Anything needed so `plugin.cc` compiles without the rest of the application (`event.hpp` and `utils.hpp` include only standard headers; checked at milestone 0). Include `<algorithm>` explicitly. Milestone 0 also fixes missing input-buffer failure handling, partial-allocation instance cleanup and the input-buffer leak during normal cleanup; looping behaviour is unchanged.
4. `plugin.cc` (Milestone 3, a core bug): `fillLoops` stops descending at a chunk that is its own source. `beginOverdub`'s "overdub using self" creates such a chunk when a loop fills its memory, and the walk never ended (an Overdub after a memory-full Multiply froze the audio thread). See [milestone-3-validation.md](milestone-3-validation.md#core-fix-endless-loop-after-a-memory-full-multiply).

Page touching and `mlock` happen in Loo-pi's worker, not in `plugin.cc`. No behaviour change to the looping itself. A bug found in the core is fixed in a separate commit, so it can be offered upstream.

## 8. Tests

### 8.1 Offline (`make test`)

`tests/host.cpp` comes from Discipline: it loads the built object, calls `run()` with random slice sizes, sets controls between slices, queues atom events, and runs the worker either at once or after a delay of several blocks.

| Test | Check |
|---|---|
| `ttl_check` | every port in `loo-pi.ttl` matches `ports.hpp`; names ASCII, at most 16 characters |
| `record_play` | record 1 s of noise, close: playback equals the recording, through the fades |
| `overdub` | a second layer adds to the first; Feedback 0.5 halves the old layer each pass |
| `undo_redo` | Undo restores the previous layer; Redo brings it back; long Undo empties |
| `press_release` | holding Overdub overdubs only while held; double-tap Rec/Dub undoes twice; long Rec/Dub clears |
| `stereo_lock` | after every command, left and right have the same state, length and position |
| `threshold` | with a threshold, recording starts on the first sample above it |
| `slicing` | the same command script gives identical output under random slicing and one slice per period |
| `events` | two commands in one block both act, at their sample times; malformed events are ignored |
| `targets` | selected, all and loop n reach the right loops; Next and Previous wrap |
| `solo` | Solo mutes the others and restores them, as SooperLooper does |
| `add_remove` | `loops` 1 to 8 and back while playing: no allocation in `run()` (checked with an allocator hook), no click, state -2 until the worker answers |
| `sync_loop` | loops synced to loop 1 record whole cycles of it and stay in phase for 10 minutes |
| `tempo` | Sync to Tempo, 120 BPM, 16 eighths: a synced loop is exactly 4 s (192,000 frames at 48 kHz) |
| `quantize` | a quantized command takes effect on the boundary; `waiting` is 1 until then |
| `commands_m3` | Multiply, Insert, Replace, Substitute, Reverse, Once, Pause and Delay each do what the spec says |
| `rate` | Rate 0.5 plays an octave down at half speed; 1 is unchanged |
| `bypass` | `enabled` 0 passes dry and mutes the loops without a click; 1 brings them back in time |
| `memory_full` | recording past the memory closes the loop cleanly, `free_mem` 0 |
| `nonfinite` | a NaN input: outputs stay finite |
| `rt_timing` | ten simulated minutes, 8 loops, random commands: worst and 99.9th-percentile `run()` time |
| `state` | save and restore four loops (a multiplied one, a muted one, an empty one): back Paused, same lengths and cycles, same audio after Trigger; the hold until Loops arrives; missing and short files; no `makePath`; restore during a worker allocation; saves on another thread while loops are removed and re-added; WAV formats |

`make test SAN=1` builds with AddressSanitizer and UBSan. The core is old C code; expect UBSan findings, and record each as fixed or accepted.

### 8.2 Desktop hosts

- `lv2lint`: no errors; document warnings that stay.
- `jalv` and Carla (both support the worker), with a guitar or a DI file.
- `tools/render` runs a WAV through Loo-pi with a script of commands at given times, for listening.

### 8.3 Ingen under WSL

With Peepo's `tools/ingen-spike/env.sh`: load Loo-pi, add loops (the worker runs), send command events by setting `commands` to an `atom:Int`, record and play a test signal, check `enabled` bypass, save and reload (controls, loop count and loop audio come back; `make test-hosts` saves with `patch:Copy` and reloads in a new engine), and run with one and four threads.

### 8.4 On the Pi

A milestone is done only after this. Build natively at the tagged commit. Play a typical patch (amp, Loo-pi with four loops, a reverb, plus a Discipline) for 30 minutes with stomps on Rec/Dub, Undo and Next loop. Record the xrun count (0 beyond the same patch without Loo-pi), DSP load, RSS, the telemetry rate with the page open, and native `rt_timing`. Results go to Peepo's `docs/evidence/`.

## 9. Milestones

"Done" means the offline tests pass on x86-64 and arm64, plus the stated host checks.

### Milestone 0: scaffolding

- `README.md` for Loo-pi; upstream's README moved to `README.sooperlooper.md`. `COPYING` stays.
- Makefile, `lv2_entry.cpp`, `ports.hpp`, `gen_ttl.py`, `loo-pi.ttl` with **the full port list of section 3**, and `ttl_check`.
- `plugin.cc` compiles alone (section 7, item 3). Loo-pi passes dry through, with loop 1 allocated and idle.
- Settle whether the worker is required.
- **Done when:** `make`, `make install DESTDIR=...` and `lv2lint` are clean, and Loo-pi loads in jalv and in Ingen under WSL. Tag `v0.0.1`, so Peepo can wire its build step early.

Implemented 19 October 2026. Offline checks pass on x86-64 (including
ASan/UBSan) and arm64 under QEMU. Staged installation, jalv and Ingen with one
and four processing threads pass. The worker remains required; its interface
schedules no jobs yet. `lv2lint` has no errors and one documented author-email
warning. The physical Pi check and release tag remain pending; see
[milestone-0-validation.md](milestone-0-validation.md).

### Milestone 1: one stereo loop

1. Section 7, items 1 and 2 are already in the milestone 0 scaffold, needed for safe per-instance allocation and an isolated core build.
2. `stereo_loop` (4.1) with press, release, long press and double tap; command controls (4.4).
3. Record, Overdub, Rec/Dub, Mute, Undo, Redo, Clear, Trigger; levels, Feedback, threshold, Fade, Safe feedback; bypass (4.6).
4. Tests: `record_play`, `overdub`, `undo_redo`, `press_release`, `stereo_lock`, `threshold`, `slicing`, `bypass`, `memory_full`, `nonfinite`, `rt_timing`.
- **Done when:** all pass, a patch in Ingen under WSL loops a test signal, and the Pi check (8.4) shows no added xruns. Tag `v0.1.0`. Peepo adds its overlay entry and pinned build; the generic page and stomp assignment (Momentary) can play it.

Implemented 20 October 2026. Offline checks pass on x86-64 with ASan/UBSan,
and arm64 under QEMU. Ingen with one and four processing threads records,
plays, bypasses and clears a stereo test signal through command controls, and
accepts single-loop Record/Undo/Redo/Clear atom events. The 600 s simulated
timing check and allocation guard pass. Native Pi/xrun evidence, the release
tag and Peepo's pinned build/overlay follow-up remain outstanding; see
[milestone-1-validation.md](milestone-1-validation.md).

### Milestone 2: several loops and the page

1. `loop_set` (4.2): targets, Next and Previous, Solo.
2. The worker (4.5) and `loops`.
3. The `commands` atom input (4.4) and the per-loop outputs (6).
4. Tests: `events`, `targets`, `solo`, `add_remove`.
5. In Peepo: the Loo-pi page, first version: loop row, quick commands, COMMANDS and LEVELS sections, telemetry and command events ([peepo-page.md](peepo-page.md)).
- **Done when:** the tests pass, four loops play on the Pi from the page and stomps, and the telemetry rate is measured. Tag `v0.2.0`.

Implemented 20 October 2026 in Loo-pi and Peepo. Worker ownership, targets,
Solo, navigation, timed commands, all telemetry groups and the first page are
implemented. Physical Pi/xrun and telemetry measurements, the release tag and
Peepo's pinned release build remain outstanding; see
[milestone-2-validation.md](milestone-2-validation.md).

### Milestone 3: everything else Beebo's Loopler does

1. Multiply, Insert, Replace, Substitute, Reverse, Once, Pause, Delay, with their special endings.
2. Quantize, Round, the quantized options; sync to Tempo (4.3) and to a loop; Relative sync.
3. Rate; the clock outputs.
4. Tests: `sync_loop`, `tempo`, `quantize`, `commands_m3`, `rate`.
5. In Peepo: RATE and GLOBAL sections, stomp binding with targets, stomp badges.
- **Done when:** the tests pass, plus a listening session on the Pi playing EDP-style (multiply and insert against a synced loop). Tag `v0.3.0`.

Implemented in Loo-pi on 20 October 2026 (items 1 to 4, plus `clocks`, `slicing` and an eight-loop timing run in `tests/test_m3.cpp`). The Peepo items (5) were done the same day: RATE and GLOBAL sections, stomp binding with targets, stomp badges. Pi listening and the release tag remain; see [milestone-3-validation.md](milestone-3-validation.md).

### Milestone 4: decisions

Saving loops through LV2 state (decided: it comes here), then the spec's remaining open questions: separate loop outputs, MIDI binding and clock. Time stretch and pitch shift are out (decided 19 October 2026). Each becomes appended ports and its own plan section.

Implemented 20 October 2026: saving loops (4.7, `tests/test_state.cpp`), checked through Ingen's own save and reload with one and four threads. Decided with the user the same day: saved loops come back Paused; no separate loop outputs for now; MIDI binding and clock wait for Peepo's MIDI. No ports were added. The Pi check (save and reload a patch with four loops, xruns while saving) and the `v0.4.0` tag remain; see [milestone-4-validation.md](milestone-4-validation.md).

## 10. Risks

| Item | Why it matters | When to settle |
|---|---|---|
| The core's age | 5,000 lines of 2002-era C, `unsigned long` positions, few comments. Bugs may show under UBSan or long sessions | Milestone 1, through `SAN=1` and `rt_timing` |
| Ported application code | `Looper` and `Engine` logic copied out of classes tied to OSC and JACK; a missed detail changes how commands feel | Each ported function names its source; tests follow SooperLooper's behaviour |
| Port count | 145 ports; Peepo's generic page must hide most of them | Milestone 1, overlay |
| Memory | 16 MB per 40 s stereo loop; 8 loops at 300 s would be 1 GB | `loop_secs` limits; measured on the Pi |
| Telemetry | 1,200 values a second while the page is open | Milestone 2 (section 6) |
| Momentary stomps | Long presses need press and release from Peepo, through the page's bindings or Momentary assignments | Milestone 1 and 2, in Peepo |
| Upstream merges | Changes to `plugin.cc` make merges harder | Keep them small and marked |
