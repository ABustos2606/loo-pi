<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
<!-- Copyright (c) 2026 Alejandro Bustos -->
# Milestone 3 validation

Recorded 20 October 2026. Milestone 3 is implemented and tested in the plugin
(the remaining commands, Quantize, sync to Tempo and to a loop, Rate and the
clock outputs) and in Peepo's page (RATE and GLOBAL, every command, Sync
switches, stomp binding and badges). The milestone is **not done**: the
listening session on the Pi and the `v0.3.0` tag are still open (see the end of
this document).

| Check | Result |
| --- | --- |
| x86-64 `make -j8 test` in Debian under WSL | PASS: every Milestone 1 and 2 test, plus `tests/test_m3.cpp` |
| `make test SAN=1` | PASS: ASan and UBSan, no findings |
| arm64 `make test` under QEMU (Debian 12 root) | PASS: the full suite, including M3 and `memory_full_multiply` |
| `make test-timing` | PASS: 600 simulated seconds each. One loop: p99.9 47 us. Eight loops: p99.9 202 us. Eight loops synced to loop 1 at Rate 0.5/1.5 with Multiply and Overdub: p99.9 196 us, max 11.2 ms. A second run gave p99.9 228 us, max 1.9 ms, so the 11.2 ms was WSL scheduling noise. No audio heap calls |
| `make lint` with lv2lint 0.17.151 | No errors; the author-email warning kept since Milestone 0 |
| Peepo's Ingen (`make test-hosts`), one and four threads | PASS |
| Peepo `ctest --preset windows` (Windows simulator) | PASS, 7 of 7 |
| Peepo's changed files with GCC (`-std=c++20 -fsyntax-only`) in Debian | No errors |

## Peepo's page

- `ui_workflows`: the RATE section edits Rate and sends Reverse to the
  selected loop. GLOBAL shows Quantize, Sync to and 8ths/cycle. Multiply sends
  a press and a release. The Sync switch works. BIND, then Insert, then
  stomp 2 makes a Loo-pi assignment (Insert, all loops) without playing, and
  draws a "2" badge on the button's corner. After binding, stomp 2 sends the
  press and the release. Binding the same command again unbinds it. Captures
  65 to 67 (Rate, Global, Bind).
- `ingen_adapter`: with 2 loops only their 12 outputs and the 3 global outputs
  are subscribed. A third loop adds its 6 outputs and keeps the known values.
  Multiply is accepted, Scratch refused.
- `physical_controls`: a Loo-pi assignment survives encoding. Scratch, a
  target past loop 8 and a non-number command are refused.
- Also fixed: on the Loo-pi page the status line overwrote every message on
  every frame, so no message there was ever readable. A new message now stays
  for 4 s.

## Behaviour covered (`tests/test_m3.cpp`)

- `commands_m3`: Multiply (three cycles), Insert, Replace, Substitute, Reverse
  (also with a long release), Once ending in Muted, Pause and the `pause`
  control, Delay with the echo at the expected frame.
- `quantize`: a quantized command waits with `lN_waiting` = 1, starts on the
  cycle boundary, Undo cancels it, and Mute quantized works.
- `tempo`: Sync to Tempo at 120 BPM with 16 eighths per cycle makes a loop of
  exactly 192000 frames.
- `sync_loop`: a loop synced to loop 1 closes on whole cycles and stays in
  phase over 10 simulated minutes.
- `rate`: 0.5 plays an octave down at half speed, 2 an octave up, and 1 is
  unchanged.
- `clocks`: 1 ms triggers on loop and cycle starts, from the close of a
  recording, in Reverse, on Multiply cycles and at Rate 0.5.
- `slicing`: output and clocks are bit-identical for 128-frame blocks, random
  slices and single frames, with sync to a loop and to Tempo, Rate, Multiply,
  quantized Reverse, Pause and Delay.
- `memory_full_multiply`: an Overdub after a Multiply that has run into the
  end of loop memory (see the core fix below).
- No allocation or free in `run()` or `work_response`, through all of the above
  and the timing run.

## Core fix: endless loop after a memory-full Multiply

The first timing run never finished. One block hung in `fillLoops`
(`src/plugin.cc`). When a loop is as long as its memory, `beginOverdub` takes
the "overdub using self" branch and sets `loop->srcloop = loop`. If that chunk
still has front or back fill pending, which it does after a Multiply,
`fillLoops` follows `srcloop` from the chunk to itself forever. This is
upstream SooperLooper's behaviour. On Peepo it would freeze Ingen's audio
thread. To reproduce: record a loop, Multiply until memory runs out (10 s at
the minimum Loop memory), then Overdub.

The fix is a commented two-line guard in `fillLoops`: the descent stops at a
chunk that is its own source. The chunk then fills from itself, which is
bounded. `memory_full_multiply` hangs without the guard (killed by a timeout)
and passes with it. This is the only change to upstream code in Milestone 3.

## Source rules and adaptations

The rules are ported from `Looper::do_event`, `Looper::run_loops`,
`Engine::process`, `Engine::generate_sync`, `Engine::calculate_tempo_frames`
and `Engine::set_tempo`. Implementation details are in
[implementation-plan.md](implementation-plan.md) sections 4.1 and 4.3.
Adaptations:

- **Rate.** Upstream resamples with libsamplerate (`SRC_LINEAR`), which
  Loo-pi does not build. Loo-pi uses its own linear interpolation. Sync pulses
  move whole to the next loop-time frame, so none are lost or split. Rate 1 is
  the unchanged direct path.
- **Tempo pulses.** A pulse due just past the end of a slice is written on the
  next slice's first sample; upstream loses it. A BPM change restarts the
  counter, as `Engine::set_tempo` does.
- **Sync to None, or to a missing loop.** Upstream gives a synced loop an
  all-zero sync buffer, so its recording would wait forever. Loo-pi runs it
  free.
- **Quantized Reverse.** The core commits the direction on the boundary but
  keeps its local rate until the run ends, so the result depended on where the
  host ended a run. While a quantized Reverse is pending, the cores run one
  frame at a time.
- **Clock outputs.** Tracked per core run from the positions before and after
  the run, then mapped back through the resampler to host frames.
- **Kept from upstream:** with Quantize on and Sync to None, a Reverse while
  playing never commits (the core needs a tempo above 0). `lN_waiting` shows
  it. [peepo-page.md](peepo-page.md) tells the page about it.

## Telemetry measured on the Pi

Measured with the Milestone 2 build and four loops playing; see
[milestone-2-validation.md](milestone-2-validation.md#on-the-pi-20-october-2026).

## Still open

1. **Peepo, small items** (see [peepo-page.md](peepo-page.md)): the
   waiting command's button doesn't blink (the loop card does), the default
   stomp bindings aren't offered, and the canvas state colour is open.
2. **The Pi:** install the new Peepo build with this plugin build, play a listening session EDP-style
   (Multiply and Insert against a synced loop), and measure xruns. The timing
   max above is from WSL; check it on the Pi.
3. **Release:** tag `v0.3.0` and pin it in Peepo's
   `tools/ingen-spike/14-more-plugins.sh`. Both are the user's to do.
