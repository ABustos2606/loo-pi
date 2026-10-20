# The Loo-pi page in Peepo

**Written:** 18 October 2026. **Follows:** [design.md](design.md). **Reference:** Beebo's Loopler screen (`C:\Pedal PI\docs\images\poly-loopler.png`) and Part II of the Beebo manual, for the features. The look is Peepo's own (Peepo `docs/03-interface-and-controls.md`, "Visual style"): Beebo's interface is a reference for concepts only, and no Beebo code, QML or assets are used.

The page is Peepo code (LVGL, `src/ui/`), not an LV2 UI: Ingen runs plugins without their interfaces, and Peepo already draws its own pages for the Tuner and the Euclidean sequencer. This document is the contract between the two repositories: what the page shows, and which ports it reads and writes. **The layout below is the design** (19 October 2026: the user left it to us). Build it as drawn; change it only for a reason found while building, and record the change here.

## What Beebo's Loopler screen has

From the screenshot, top to bottom:

- **Section tabs** on the left: COMMANDS, LEVEL / SYNC, RATE.
- **The open section.** LEVEL / SYNC shows a wide bar for the highlighted value (WET 0.00), tiles for Input gain, Threshold, Feedback and Wet (the highlighted one in yellow), and on the right the switches Sync, Play sync, Play feedback.
- **The loop row:** one card per loop, scrolling sideways. Each card has a state icon, LOOP n, CYCLE in seconds, TOTAL in seconds, the state (PLAYING, MUTED ...) and a position bar. The selected loop has a bright border.
- **The bottom bar:** home, MIDI binding, global settings (globe), then Play, Pause, Record, and Clear (bin).

From the manual: commands act on the selected loop, all loops or a given loop; global settings hold Quantize, Previous loop and Next loop; a command is bound to a footswitch by holding the switch and pressing the command, and to MIDI through the MIDI icon.

## The page

800 x 480, Peepo's frame: header with breadcrumb, title and pills; footer with Back, a message line, BPM and CPU.

```
+------------------------------------------------------------------------------+
| o PEEPO // MODULE                                  [COMMANDS][LEVELS][RATE]  |
| LOO-PI                                             [GLOBAL] [ON] [ACTIONS]   |
+------------------------------------------------------------------------------+
|  section content (see below)                                       ~150 px   |
|                                                                              |
+------------------------------------------------------------------------------+
| +-----------+ +-----------+ +-----------+ +-----------+ +---               |
| | > LOOP 1  | | > LOOP 2  | | ~ LOOP 3  | | o LOOP 4  | |                   |
| | 3.92 s    | | 7.16 s    | | 9.28 s    | | --        | |                   |
| | of 3.92 s | | of 7.16 s | | of 9.28 s | |           | |      ~120 px      |
| | PLAYING   | | OVERDUB   | | MUTED     | | EMPTY     | |                   |
| | ======--- | | ==------- | | ========- | |           | |                   |
| +-----------+ +-----------+ +-----------+ +-----------+ +---               |
|  [REC/DUB]  [OVERDUB]  [PLAY]  [PAUSE]  [UNDO]  [CLEAR]        [+ LOOP]      |
+------------------------------------------------------------------------------+
| < BACK   LOOP 1 // PLAYING // SYNC TEMPO                     120 BPM  12 CPU |
+------------------------------------------------------------------------------+
```

- **Header.** `PEEPO // MODULE`, the title LOO-PI, and pills for the four sections, then ON (bypass) and ACTIONS, as on every module page. If six pills do not fit beside the title, the sections move to a column on the left of the content, as on Beebo's screen.
- **The loop row** is always visible. One card per loop, about four on screen, scrolling sideways. A card shows the loop number, the time into the loop and its length, the cycle length when it differs, the state in words and a position bar. The selected loop has the white border Peepo uses for a pick. Tap a card to select its loop.
- **State colours.** Colour only where it means something: the state word and the position bar take the state's colour (for example recording red, overdubbing orange, playing in the module's TIME group colour, muted grey, waiting blinking). An empty loop is dim.
- **Quick commands** under the row, for the selected loop: Rec/Dub (white, the page's main action), Overdub, Play (Trigger), Pause, Undo, Clear, and **+ Loop** to add a loop.
- **Footer.** The message line shows the selected loop, its state and the sync source.

### Sections

**COMMANDS.** Every command, as buttons in two rows of eight: Record, Overdub, Multiply, Insert, Replace, Substitute, Reverse, Mute, Undo, Redo, Trigger, Once, Pause, Solo, Delay, Clear. Above them, **Act on**: SELECTED, ALL, or LOOP n. A button sends a press when touched and a release when let go, so holding works as on a footswitch. A command waiting for sync blinks its button.

**LEVELS** (Beebo's LEVEL / SYNC), for the selected loop: bars for Loop level, Input gain, Rec threshold and Feedback; switches for Sync, Play sync and Play feedback. Bars and switches behave as on every Peepo module page (drag sideways, E1 picks, E2 turns).

**RATE**, for the selected loop: Rate (0.25x to 4x, with 1x marked) and Reverse. Plain speed only: pitch follows speed (decided 19 October 2026; no stretch or pitch shift).

**GLOBAL** (Beebo's globe): Dry level; Quantize, Round, Mute quantized, Overdub quantized, Replace quantized; Sync to, Relative sync, 8ths per cycle; Fade, Safety feedback; Loop memory; Previous loop and Next loop; Remove loop. The BPM is the patch tempo and is not repeated here.

### Encoders and stomps

- **Encoders**, as elsewhere: E1 moves the highlight (header pills, section controls, loop cards, quick commands), its push acts, its hold goes Back; E2 turns the highlighted value. On the loop row, E2 changes the selected loop.
- **Binding a stomp.** Beebo's way is to hold the footswitch and press the command. Peepo's stomps act the moment they are pressed, so Peepo binds like its own module assignment: tap **BIND** (on the COMMANDS section), pick Act on, tap a command, then press a stomp. A stomp pressed while binding does not play. Pressing it again on the same command removes the binding.
- A bound stomp sends the command's press when it goes down and its release when it comes up, so long presses (Overdub held, Undo held for Undo all) work.
- **Stomp badges.** A command bound to a stomp shows the stomp's number in a white dot, the way the patch shows stomp badges on modules.
- A useful default for a new Loo-pi, offered and not forced: stomp 1 Rec/Dub, stomp 2 Undo (held: Clear), stomp 3 Next loop.

## The contract

### What the page writes

| Action | How |
|---|---|
| A command, with target and press or release | One event on Loo-pi's `commands` atom input (below) |
| A setting | The control port, as for any module, so the patch saves it |
| Select a loop | The `selected` control port |
| Add or remove a loop | The `loops` control port; the loop appears when its memory is ready (`lN_state` leaves "unallocated") |

**Command events.** Ingen appends any value set on an atom sequence input as an event in the next block (`reference/upstream/ingen/src/server/events/SetPortValue.cpp:114`). The page sets the `commands` port to an `atom:Int`:

Milestone 2 accepts the implemented recording/history/mute/trigger commands,
Solo and its navigation variants, and targets selected, all and loops 1 to 8.
All eight loop telemetry groups are live when allocated. Unsupported Milestone
3 commands are ignored. Peepo sends typed `xsd:int` literals through Ingen's
Sratom reader, which produces `atom:Int`; the compact integer contract is kept.

```
value = target * 256 + command * 2 + press
    target:  0 selected, 1 all, 1 + N for loop N (1 to 8)
    command: SooperLooper's Event::command_t number (src/event.hpp),
             64 Next loop, 65 Previous loop
    press:   1 for press, 0 for release
```

Next and Previous are controls in upstream `Engine`, rather than entries in
`Event::command_t`. Loo-pi reserves wire IDs 64 and 65 outside that enum;
these IDs are never cast to the upstream enum. Only their presses navigate.
Targets for absent loops are ignored, including navigation with an invalid target.

`selected_out` is authoritative after navigation commands. Peepo reflects a
changed selection into the `selected` setting so it is saved, and a later tap
on the old setting value still reaches the plugin. Touch commands pin the
selected target until release, including release after a lost press or leaving
the page. Telemetry updates widgets without rebuilding them; subscriptions
clear on leaving the page or disconnecting. Transient atom input values never
dirty the graph.

**Measured on the Pi (20 October 2026):** Ingen sends every subscribed output
25 times a second whether or not it changed: 51 outputs, 1,275 values and
203 KiB a second, also for loops that are not active. With the page open,
Ingen's CPU rose from 6.4 % to 15.5 % of a core, the interface's from 0.4 % to
18.4 %, and the DSP peak from 5 % to 29 %
([milestone-2-validation.md](milestone-2-validation.md#on-the-pi-20-october-2026)).
Since Milestone 3 the page subscribes only to the loops that exist (27 outputs
with 4 loops). Adding a loop subscribes its outputs and keeps the values
already known.

**Milestone 3 (Loo-pi side, 20 October 2026).** The `commands` input accepts
every `Event::command_t` number except Scratch (4) and the mute-mode toggles
Redo toggle, Quantize toggle and Round toggle (11 to 13), which are ignored.
Multiply, Insert, Replace, Substitute, Reverse, Once, Pause and Delay work, as
do Mute on/off, Pause on/off, Mute trigger, Set and Reset sync position. The
`pause` control is live. Quantize, Round, the quantized options, Sync to,
Relative sync, 8ths/cycle, BPM, each loop's Sync, Play sync and Rate are live,
and so are the clock outputs. `lN_waiting` is 1 while a quantized or synced
command waits; `lN_next` is the state it will enter.

**Milestone 3 (Peepo side, 20 October 2026).**
- **Commands:** every command button is enabled. `looper_command_supported`
  (`src/core/looper.hpp`) matches the list above.
- **Header:** four section pills (Commands, Levels, Rate, Global) plus ON and
  ACTIONS.
- **LEVELS:** has live Sync and Play sync switches.
- **RATE:** Rate and a Reverse button, both for the selected loop.
- **GLOBAL:** Dry level, Quantize, Round, the three quantized options, Sync to,
  Relative sync, 8ths/cycle, Fade, Safe feedback, Loop memory, Previous loop,
  Next loop and Remove loop.
- **Footer:** shows the sync source (FREE, SYNC TEMPO or SYNC LOOP n). A new
  message stays for 4 s before the status line replaces it.
- **Binding:** BIND, then Act on, then a command, then a stomp. The stomp
  doesn't play while binding, and pressing it again on the same command and
  target unbinds it.
- **Stomp badges:** a bound command shows its stomp numbers in a white dot, for
  the current Act on, and the stomp also shows on the module's square on the
  patch.
- **Stomp press and release:** a bound stomp sends the press when it goes down
  and the release when it comes up. A SELECTED target is fixed at the press
  while the page is open; otherwise Loo-pi's own selection (target 0) is used.

Not done yet: a waiting command's button doesn't blink (the loop card does), the
default stomp bindings aren't offered, and item 6 below is still open.

Note for the page: with Quantize on and Sync to None, a Reverse while playing
never commits (upstream: the core needs a tempo above 0, and the engine passes
0 without a sync source); `lN_waiting` shows it.

### What the page reads

Telemetry, watched only while the page is open, like the Tuner's `freq_out`. N is the loop number, 1 to 8 (port list in [implementation-plan.md](implementation-plan.md#3-ports)):

| Port | Meaning |
|---|---|
| `lN_state` | SooperLooper's state number (`src/plugin.hpp`, `LooperState`), or -2 for a loop with no memory yet |
| `lN_next` | the state after a waiting command |
| `lN_waiting` | 1 while a command waits for sync or quantize |
| `lN_len` | loop length, seconds |
| `lN_cycle` | cycle length, seconds |
| `lN_pos` | position, seconds |
| `selected_out`, `soloed`, `free_mem` | the selected loop as Loo-pi sees it, the soloed loop (0 for none), seconds of memory left in the selected loop |

Settings are read from the graph, as for any module.

### Peepo-side work

1. A Loo-pi page in `src/ui/application.cpp` (or its own file), chosen by the plugin URI, as the Euclidean page is (`src/core/euclidean.hpp`).
2. `watch_looper` / `looper_status` in the engine interface and the Ingen session, following `watch_euclidean`.
3. Sending `atom:Int` values to an atom input port through the Ingen adapter.
4. Stomp bindings to Loo-pi commands, stored per patch next to the existing assignments in the same eight fields: `peepo:control-stomp1 "looper|m4|<uri>|<command>|<target>|<target>|0|momentary"`. `ControlAction::Looper` in Peepo's model keeps the command in the parameter field and the target in both range ends. Sent as press and release. Done in Milestone 3.
5. The overlay entry: name, category, `"bypass": "port"`, `"tempo": "bpm"`, and hiding the generic page's controls the Loo-pi page replaces.
6. The canvas square could show the selected loop's state colour, so the patch shows the looper at a glance (**open**).
7. Tests: LVGL captures at 800 x 480 of empty, recording, several loops playing, muted, waiting; adapter tests for the telemetry and the command events; a live WSL check with the real plugin.
