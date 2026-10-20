<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
<!-- Copyright (c) 2026 Alejandro Bustos -->
# Milestone 1 validation

Recorded 20 October 2026. One stereo loop is implemented. Physical Pi evidence
and the release/integration steps remain outstanding, so the milestone's
hardware completion gate is not checked off.

| Check | Result |
| --- | --- |
| x86-64 `make -j2 test` in Debian under WSL | PASS: metadata, ABI, lifecycle, core behavior and LV2 integration |
| `make -j2 test SAN=1` | PASS: ASan and UBSan, with UBSan configured to halt on a finding |
| arm64 `make -j2 test BUILD_DIR=build/arm64 OPT="-O2 -mcpu=cortex-a76"` | PASS under QEMU in the existing Debian 12 root |
| `make test-timing` | PASS: 600 simulated seconds, one stereo loop, 128 frames at 48 kHz, random commands, no audio heap calls |
| `make lint` with lv2lint 0.17.151 | No errors; author-email warning retained from Milestone 0 |
| jalv | PASS: discovery, required features, stereo dry audio |
| Peepo's patched Ingen, one and four processing threads | PASS: stereo recording/playback from control and atom commands, playback with live input silent, bypass/restore, Undo, Redo and Clear |

## Behavior covered

- Record one second of deterministic stereo noise and compare playback with
  the source away from crossfades; Trigger restarts playback at its start.
- Overdub adds a layer. Feedback 0.5 halves the previous content each pass.
  Undo restores the previous version; Redo brings back the layer. Clear is
  Undo all and leaves the history available to Redo all.
- Releases after strictly more than one second end held commands; exactly
  one second remains a short press. Long Undo/Redo use their all variants,
  and long Rec/Dub clears. A release does not restart a command already in
  Play. Frame-zero presses and Rec/Dub double taps across release are covered.
- Every tested command preserves left/right state, length and position.
  Threshold starts recording on the exact positive left-channel crossing,
  and the right channel starts at the same sample through the leader sync.
- Input gain scales the recording send, Loop level scales playback, Safety
  feedback applies the core's 0.96 factor, and Play feedback decays playback.
- The same script produces bit-identical output under regular 128-frame and
  random slices, including gain/feedback changes, history fades and bypass.
- Bypass fades to silence over 10 ms, preserves loop position and contents,
  restores playback in time, and keeps an already muted loop muted. A constant
  test loop has no bypass transition jump larger than 0.001. Dry remains at
  Dry level. Loop times also pass at 44.1 and 96 kHz.
- Recording fills the exact power-of-two allocation and closes into Play;
  free memory reaches zero. The capacity test uses a short private test loop
  to exercise the boundary rapidly; the plugin still allocates its 40 s default.
- NaN/Inf audio and controls stay finite at the outputs. Finite input large
  enough to overflow during overdub silences the affected slice and logically
  clears the loop. No bulk erase of loop audio occurs.
- Advertised buffer maxima of 1, 37, 2048 and 32768 frames and deliberately
  oversized 40000-frame calls respect scratch and output buffer bounds.
- Atom commands land at their sample timestamps, including two Record
  commands in one block. Selected/all/loop-1 targets, dedicated Redo and Clear,
  and malformed, truncated, unknown and later-milestone commands are covered.

The allocation guard wraps malloc/calloc/realloc/free and C++ new/delete in
the executable linked with the actual stereo layer and core. It observes no
heap calls during those audio paths. Source inspection of LV2 run and command
parsing also finds no allocation, locks, file/console I/O or loop-buffer clears.
Memory and scratch storage are allocated and touched during instantiation.

## Source rules and adapters

`StereoLoop` cites `Looper::initialize`, `Looper::do_event`, `Looper::run` and
`Looper::run_loops`. It keeps two cores, channel-0 sync feeding channel 1,
private scalar/control outputs for each core, and the source press/release
translations. The core's Multi input is reset to -1 between commands.

Three adaptations are needed to meet the design on Ingen:

1. Separate held and last-press timestamps avoid treating sample zero as an
   absent press and retain Rec/Dub double-tap timing across a release. The
   upstream code clears its one timestamp on release. The core's UNDO_TWICE
   shares its one-Undo implementation, so Loo-pi issues the second Undo after
   the first fade finishes. Tests require two layers to be undone.
2. Linear smoothing has a fixed 10 ms sample duration, rather than depending
   on each host slice's size. Loop gain is applied outside the core. A
   changing Feedback steps every 32 samples on boundaries counted from the
   start, and core calls never cross a step while it ramps; history fades use
   one-sample core calls. Neither effect shifts with block boundaries. Steady
   audio stays on the chunked path. (Changed after review on 20 October 2026:
   one-sample calls for Feedback cost 2.7 times as much under an LFO.) No new changes to upstream files were needed in Milestone 1.
3. Dedicated Record, Redo and Clear lack control ports, so the single-loop
   atom path was brought forward from Milestone 2. It uses the existing event
   encoding, validates integer command numbers before casting to the upstream
   enum, and ignores absent-loop targets and unsupported commands. The parser's
   malformed-enum case exposed a UBSan finding during development; the bounds
   check fixes it and the final strict sanitizer run is clean.

The command queue is bounded and requires no allocation. Coincident requests
retain input order and are serviced on successive samples; an identical Multi
command gets its required intervening idle sample. Loop 1 publishes real
telemetry. Other loop outputs remain unallocated (-2), and clocks are silent.
Free memory reports capacity remaining for extending the current loop; undo
history shares the allocation and can be reclaimed by the core as recording
continues.

## Reproduction and remaining gates

Run the commands in the table. The host test uses its own named dummy JACK
server and private Ingen sockets; logs and protocol transcripts stay in
`build/host-smoke/`. Source Peepo's `tools/ingen-spike/env.sh` for its Ingen
library paths, and pass `HOST_ARGS=--ingen=/opt/peepo/bin/ingen`. The local
validator used here is Discipline's existing lv2lint build; its online, ELF
and X11 checks are disabled, as recorded in Milestone 0. Symbol visibility and
dependencies are checked separately.

The native x86-64 timing run measured 60.384 us at the 99.9th percentile and
715.263 us maximum per 128-frame call (1.482 s wall time for 600 simulated
seconds). These observations include WSL scheduling and are not realtime
deadlines or Pi measurements. This milestone measures one loop; the planned
eight-loop stress case belongs to Milestone 2. Emulated arm64 tests validate
functionality, not Pi timing. The temporary arm64 workspace mount is removed
after testing.

The physical Pi check from implementation-plan section 8.4 still needs to be
run and recorded in Peepo's evidence. A four-loop patch requires Milestone 2;
Milestone 1 hardware validation should use its implemented one loop, then
repeat the four-loop stress check when available. No `v0.1.0` tag has been
created. Tag the reviewed commit after the hardware gate, then add Peepo's
pinned build and catalogue overlay with `bypass: port`, tempo port `bpm` and
momentary stomp bindings. The Peepo page remains Milestone 2 work.
