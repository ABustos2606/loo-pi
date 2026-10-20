<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
<!-- Copyright (c) 2026 Alejandro Bustos -->
# Milestone 2 validation

Recorded 20 October 2026. Several loops and Peepo's first Loo-pi page are
implemented. Physical Pi evidence, the telemetry-rate measurement and the
release/integration steps remain outstanding, so the milestone's hardware
completion gate is not checked off.

| Check | Result |
| --- | --- |
| x86-64 `make -j4 test` in Debian under WSL | PASS: metadata, ABI, lifecycle, core, LV2 integration and multi-loop |
| `make -j4 test SAN=1` | PASS: ASan and UBSan, with UBSan configured to halt on a finding |
| arm64 `make -j4 test BUILD_DIR=build/arm64 OPT="-O2 -mcpu=cortex-a76"` | PASS under QEMU in the existing Debian 12 root; AArch64 library |
| `make test-timing` | PASS: 600 simulated seconds each for one and eight stereo loops, 128 frames at 48 kHz, random commands, no audio heap calls |
| `make lint` with lv2lint 0.17.151 | No errors; author-email warning retained from Milestone 0 |
| jalv | PASS: discovery, required features, stereo dry audio |
| Peepo's patched Ingen, one and four processing threads | PASS: four loops allocated by Ingen's worker, atom targets, stereo mix, explicit-loop Solo and restore, removal of playing loops, controls, atoms and bypass |
| Peepo `ctest --preset windows` (Windows simulator) | PASS, 7 of 7: Ingen adapter telemetry/command encoding and the page workflow |

## Behavior covered

- `loops` 1 to 8 and back while playing: one worker job per added loop,
  state -2 until the response arrives, delayed work and delayed responses,
  re-adding while a free is still queued, a late allocation after removal,
  and a rejected `schedule_work` that leaves ownership on the audio side and
  is retried.
- Allocation failure injected at each heap call of the worker's allocation
  (object, scratch and partial core allocations): the loop reports -2, does
  not reschedule every block, and a later `loops` change retries it.
- Removing loops fades their returns over 10 ms with bounded sample steps and
  is silent before the worker frees them. Nothing is freed on the audio thread.
- Targets: selected, all and loop N reach the right loops; targets for absent
  loops are ignored. Next and Previous wrap within the active loops, through
  atoms (IDs 64 and 65) and the Next loop control. A saved `selected` value
  does not reset a navigation made by command.
- Solo follows `Engine::do_global_rt_event`: it mutes the other loops and
  restores them, keeps pre-muted loops muted, handles long releases, Solo
  next/previous and a soloed loop being removed. A loop that arrives during
  Solo starts muted.
- The same multi-loop script gives bit-identical stereo output under random
  slicing and regular 128-frame blocks, including targets, Solo, history and
  mute.
- A non-finite return from one loop clears that loop only; a finite sum that
  overflows silences the host slice.
- Peepo: the adapter subscribes to the 51 telemetry outputs only while the
  page is shown and unsubscribes on leaving it; stable values do not expire
  (Ingen broadcasts changes only); commands are sent as `xsd:int`, which
  Ingen's Sratom reader turns into `atom:Int`; transient `commands` values do
  not dirty the saved graph. The page shows loop cards with live state,
  position and cycle, quick commands with press and release pinned to the
  loop that got the press, the target selector, add/remove loop, and the
  LEVELS section for the selected loop. Telemetry updates widgets without
  rebuilding the page.

## Source rules and adapters

`LoopSet` cites `Engine::do_global_rt_event` and `Looper::set_soloed`.
Two adaptations:

1. Upstream's pre-solo mute test ORs Muted with OffMuted and is always true;
   Loo-pi uses the intended AND so pre-muted loops stay muted on restore, as
   the Milestone 1 bypass adapter already does.
2. Next and Previous are Engine controls upstream, not `Event::command_t`
   entries. Loo-pi gives them wire IDs 64 and 65 outside the enum and never
   casts them to it.

`StereoLoop` gains `Looper::finish_state` (for the exclusive record
commands) and the `RECORD_OVERDUB_END_SOLO` long-press translation. Loop
memory is touched and `mlock`ed best effort on the worker; hosts commonly
have a small `RLIMIT_MEMLOCK`, so a failed lock is ignored. No upstream file
changed in this milestone.

`loop_secs` applies to loops allocated after it changes; loop 1 is allocated
at instantiation with 40 s, rounded up by the core.

## Reproduction and remaining gates

Run the commands in the table, as in Milestone 1: source Peepo's
`tools/ingen-spike/env.sh` and pass `HOST_ARGS=--ingen=/opt/peepo/bin/ingen`
for the host test, and `LV2LINT=` Discipline's local lv2lint build for
`make lint`. The arm64 run copied the sources (without `build/` and `.git`)
into `/opt/peepo-arm64/tmp`, ran the tests in the chroot and removed the copy.
Peepo's checks are its Visual Studio preset: `cmake --build --preset windows`
then `ctest --preset windows`; screenshots `60` to `64` in
`build/screenshots/` show the page.

The native x86-64 eight-loop timing run measured 214.3 us at the 99.9th
percentile and 7644.6 us maximum per 128-frame call. The maximum is above a
128-frame period at 48 kHz (2.67 ms); it includes WSL scheduling and must be
checked natively on the Pi before this milestone is called done. Emulated
arm64 tests validate function, not timing. JACK under WSL reports that it
cannot lock memory; this is a WSL limit, not a Loo-pi result.

Outstanding, from implementation-plan section 8.4 and the milestone's done
criteria:

- Four loops played on the Pi from the page and stomps, with xrun count, DSP
  load, RSS and native `rt_timing` recorded in Peepo's `docs/evidence/`.
- The telemetry rate with the page open (plan section 6).
- Tag `v0.2.0` on the reviewed commit after the hardware gate, then pin it in
  Peepo's build (`tools/ingen-spike/14-more-plugins.sh`,
  `reference/manifest.json`). The `v0.0.1` and `v0.1.0` tags are not created
  yet either.
