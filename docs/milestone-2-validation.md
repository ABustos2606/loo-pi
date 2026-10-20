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

## On the Pi, 20 October 2026

Loo-pi `45c301f` and the matching Peepo interface (working tree on top of
`31ff8c7`, built under arm64 emulation) were installed on the Pi with a
targeted copy, not `install.sh`. The PC's arm64 root lacks the GxPlugins and
Dragonfly bundles installed on the Pi, and `install.sh` replaces the whole
`/opt/peepo`. Backups are in `/opt/peepo/backup-loopi-20261020/`.

Patch: IN, Loo-pi with 4 loops (40 s each) playing, OUT. JACK at 48 kHz,
128 frames, 3 periods (`jackd -R -P 70 -d alsa -d hw:CODEC`). The user played
from the page; nothing was audibly wrong. Measurements were passive: xruns
counted from JACK's journal (`JackEngine::XRun: client = ingen was not
finished`), DSP load from a portless JACK client, CPU and RSS from `/proc`,
telemetry from `strace -f` on the interface's engine socket.

| | Page open (60 s) | Page closed (180 s) |
|---|---|---|
| Telemetry to the interface | 1,275 values/s, 203 KiB/s | none |
| xruns | 4 (4 per minute) | 2 (0.7 per minute) |
| JACK DSP load, median / peak | 5.0 % / 28.7 % | 4.7 % / 5.2 % |
| Ingen CPU (one core) / RSS | 15.5 % / 202 MB | 6.4 % / 202 MB |
| Interface CPU (one core) / RSS | 18.4 % / 14 MB | 0.4 % / 14 MB |

From the engine restart at 06:12 to 06:24, with the page open for at least
part of the time, JACK logged 18 xruns in 11.5 minutes.

Findings:

- **Loo-pi's audio processing is cheap.** Four playing loops cost Ingen about
  6 % of one core, with a flat 5 % DSP load.
- **Telemetry is the cost, not the looping.** Ingen sends every one of the 51
  monitored outputs (6 per loop for all 8 loops, `selected_out`, `soloed`,
  `free_mem`) 25 times a second, **whether or not the value changed**. Loops
  5 to 8, which were not active, were sent as often as the playing loops.
  The plan's estimate (1,200 values/s) was right; the page contract's
  assumption that Ingen sends changes only was wrong (corrected in
  [peepo-page.md](peepo-page.md)). With the page open, Ingen's CPU rises
  2.4 times, the interface goes from idle to 18 % of a core, the DSP peak goes
  from 5 % to 29 %, and the xrun rate appears to rise. The xrun counts are
  small, so that last point is an indication, not a proof.
- The Pi already had xrun bursts on 19 October with other patches and no
  Loo-pi (00:28 to 01:08, 14:24 to 15:42, 18:13 to 18:19). Whether the 0.7 per
  minute with the page closed is Loo-pi's, needs the same measurement with
  plain IN to OUT (not done yet).

Suggested Peepo-side changes, not made (Peepo is outside this repository's
work):

1. Subscribe only to the loops that exist (`loops`), plus the three global
   outputs: 27 outputs instead of 51 with 4 loops, about half the traffic.
2. If that is not enough, the fallbacks of plan section 6: watch only
   `lN_pos` for the cards on screen, or an appended atom output in Loo-pi
   that sends a compact message per loop only when something changes.

Not yet measured on the Pi: the baseline without Loo-pi, the plan's typical
patch (amp, reverb, Discipline) for 30 minutes with stomps, native
`rt_timing`.

How to repeat, from the PC (`pedalpi` in `~/.ssh/config`):

- xruns in the last 10 minutes:
  `ssh pedalpi "journalctl -u peepo-jack --since '-10min' | grep -c XRun"`.
- Load, CPU and RSS over N seconds: a portless JACK client that samples
  `cpu_load()` once a second and reads `/proc/<pid>/stat` and `status` for
  `ingen` and `peepo`. Its xrun callback missed some xruns that JACK logged;
  count xruns from the journal.
- Telemetry: `strace -f -p $(pgrep -o -x peepo) -e trace=recvfrom -s 65536`
  for 20 s, then count the port symbols (`l1_pos` and so on) in the capture.
  Without `-f` the capture misses the thread that reads the engine socket.

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

- Four loops played on the Pi from the page: done, see above. Still to do:
  stomps, the baseline without Loo-pi, the typical patch for 30 minutes and
  native `rt_timing`. Results are recorded here rather than in Peepo.
- The telemetry rate with the page open: measured, see above. Reducing it is
  a Peepo change.
- Tag `v0.2.0` on the reviewed commit after the hardware gate, then pin it in
  Peepo's build (`tools/ingen-spike/14-more-plugins.sh`,
  `reference/manifest.json`). The `v0.0.1` and `v0.1.0` tags are not created
  yet either.
