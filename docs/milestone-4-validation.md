<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
<!-- Copyright (c) 2026 Alejandro Bustos -->
# Milestone 4 validation

Recorded 20 October 2026. Milestone 4 is implemented in the plugin: loops are
saved with the patch through LV2 state, and the spec's remaining open
questions are decided. The milestone is **not done**: the Pi check and the
`v0.4.0` tag are still open (see the end of this document).

## Decisions (with the user, 20 October 2026)

| Question | Decision |
| --- | --- |
| Saving loops | Done: LV2 state, one WAV file per loop ([implementation-plan.md](implementation-plan.md#47-saving-loops-lv2-state)) |
| State of reloaded loops | Paused, as `Looper::load_loop` leaves a loop loaded into an empty one. Trigger on all loops starts them together |
| Separate loop outputs | Not now. Reopen when a patch needs a loop on its own effect |
| MIDI binding and MIDI clock | Wait for Peepo's MIDI. The `commands` input can accept `midi:MidiEvent` later without new ports |

No ports were added. The plugin now declares `state:interface`, uses
`state:mapPath`, `state:makePath` and `state:freePath` when offered, and is
`lv2:minorVersion 4`.

## Checks

| Check | Result |
| --- | --- |
| x86-64 `make -j8 test` in Debian under WSL | PASS: every earlier test, plus `tests/test_state.cpp` |
| `make test SAN=1` | PASS: ASan (with leak checks) and UBSan, no findings, including the concurrent save |
| arm64 `make test BUILD_DIR=build/arm64 OPT="-O2 -mcpu=cortex-a76"` under QEMU (Debian 12 root) | PASS: the full suite |
| `make test-timing`, run against the Milestone 3 commit back to back | No change. One loop: p99.9 32 to 45 us (M3 build 42 to 56 us). Eight loops: 191 to 207 us (200 to 208 us). Eight synced loops at Rate 0.5/1.5: 159 to 223 us (207 to 219 us). Maxima of 2 to 12 ms in both builds are WSL scheduling noise |
| `make lint` with lv2lint 0.17.151 | No errors; the author-email warning kept since Milestone 0 |
| Peepo's Ingen (`make test-hosts`), one and four threads | PASS, including the new save and reload below |

## Through Ingen

`make test-hosts` now saves the patch the way Peepo does (`patch:Copy` from
`ingen:/main` to a `file:` bundle) with two recorded loops. It checks that the
bundle has `loopi/state.ttl`, `loopi/loop1.wav` and `loopi/loop2.wav`, stops
the engine, starts a new one, and loads the bundle (`patch:Copy` back to
`ingen:/main`). The output is silent (the loops are Paused) until Trigger on all
loops, and then is the same mix as before saving. Ingen wrote:

```
state:state [
    <https://github.com/ABustos2606/loo-pi#loops> "2"^^xsd:int ;
    <https://github.com/ABustos2606/loo-pi#loop1> <loop1.wav> ;
    <https://github.com/ABustos2606/loo-pi#loop1_cycle> "10484"^^xsd:long ;
    <https://github.com/ABustos2606/loo-pi#loop1_memory> "40.0"^^xsd:float ;
    ...
] .
```

The paths are relative to the state file, which Peepo's missing-file check
already follows (Peepo's `docs/17-patch-storage.md`).

## Behaviour covered (`tests/test_state.cpp`)

- `round_trip`: four loops (a plain one, an empty one, a multiplied one with
  several cycles, a muted one) saved and restored into a new instance. Restored
  loops are Paused with the same lengths and cycles; the empty loop is
  allocated by the worker. After Trigger on all loops the output equals the
  original's (within 1e-6) from the end of the fades on. Every path from
  `mapPath` and `makePath` is freed with `freePath`.
- The hold: with `loops` still at its default of 1 after the restore (as
  Ingen sets it later), the restored loops stay; once `loops` reaches the saved
  4, the control acts as usual. A different value also ends the hold.
- `broken files`: a missing file and a truncated file give empty loops. A
  save without `makePath` stores nothing; restoring an empty state changes
  nothing.
- `pending`: a restore while the worker is allocating keeps the restored loop
  and sends the late allocation back to the worker to be freed.
- `concurrent save`: thousands of saves on one thread while another runs the
  plugin, removes and re-adds loops and overdubs. Every saved file reads back;
  ASan finds no use after free; no allocation in `run()`.
- `wav`: float written and read back, 16 and 24-bit PCM, mono to both
  channels, unknown chunks skipped.

## Source rules and adaptations

The rules are ported from `Looper::save_loop`, `Looper::load_loop` and
`sl_read_current_loop_audio`. Details are in
[implementation-plan.md](implementation-plan.md#47-saving-loops-lv2-state).
Adaptations:

- **WAV files without libsndfile.** Written as 32-bit float (upstream's
  default format); read as 16, 24 or 32-bit PCM or 32-bit float.
- **Saving while the loop runs.** Upstream saves from its non-audio thread
  without a lock. Loo-pi copies against a geometry published by the audio
  thread and checks that it held, so a loop being moved or cleared at that
  moment is copied again. Samples an Overdub writes during the copy may be old
  or new.
- **Cycles.** Upstream loads every file as one cycle; Loo-pi keeps the saved
  cycle when it divides the loop.
- **The saved loop count is held** until the `loops` control arrives, because
  Ingen restores state before it sets control values.
- Undo history is not saved, as upstream. Another sample rate is not
  converted: the loop plays at a new pitch.

## Still open

1. **The Pi:** save and reload a patch with four 40 s loops on the card. Note
   how long the save takes, the bundle size (about 15 MB per loop, in every
   revision), and xruns while saving (the copy runs beside the audio thread).
2. **Peepo, optional:** a PLAY ALL action on the page after a patch loads with
   paused loops ([peepo-page.md](peepo-page.md)).
3. **Release:** tag `v0.4.0` and pin it in Peepo's
   `tools/ingen-spike/14-more-plugins.sh`. Both are the user's to do.
