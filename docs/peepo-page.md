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

Milestone 1 already accepts Record, Overdub, Rec/Dub, Mute, Undo, Redo,
Clear (Undo all), Redo all and Trigger for targets selected, all and loop 1.
This subset was brought forward to expose commands without dedicated control
ports. Other targets and commands remain inert until their milestones.
Loop 1's telemetry is live; loops 2 to 8 still report unallocated. The page
itself and routing to several loops remain Milestone 2 work.

```
value = target * 256 + command * 2 + press
    target:  0 selected, 1 all, 1 + N for loop N (1 to 8)
    command: SooperLooper's Event::command_t number (src/event.hpp)
    press:   1 for press, 0 for release
```

An int is the smallest change to Peepo's adapter, which already sends floats and URIs. If the adapter can send `atom:Object`s, a `patch:Set`-style object is cleaner; decide in milestone 2, before any patch depends on it (events are not saved, so this can change later without breaking patches).

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
4. Stomp bindings to Loo-pi commands, stored per patch next to the existing assignments (for example `peepo:control-stomp1 "looper|m4|<uri>|<command>|<target>"`), and sent as press and release.
5. The overlay entry: name, category, `"bypass": "port"`, `"tempo": "bpm"`, and hiding the generic page's controls the Loo-pi page replaces.
6. The canvas square could show the selected loop's state colour, so the patch shows the looper at a glance (**open**).
7. Tests: LVGL captures at 800 x 480 of empty, recording, several loops playing, muted, waiting; adapter tests for the telemetry and the command events; a live WSL check with the real plugin.
