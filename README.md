# Loo-pi

A stereo, multi-loop LV2 looper for Peepo and other LV2 hosts: up to 8 loops with record, overdub, multiply, insert, replace, substitute, reverse, undo and redo, solo, one-switch Rec/Dub, and sync to the patch tempo or to another loop. Peepo shows it on a page of its own.

Loo-pi is made from **SooperLooper** by Jesse Chappell (https://github.com/essej/sooperlooper). It keeps SooperLooper's looping core and replaces the standalone JACK application around it with an LV2 plugin. GPL-2.0-or-later, like SooperLooper; see [COPYING](COPYING).

**Status, 20 October 2026:** Milestone 1 is implemented: one stereo loop with
Record, Overdub, Rec/Dub, Mute, Undo, Redo, Clear and Trigger; press/release,
long presses and double taps; dry/loop levels, input gain, threshold, feedback,
Fade, Safety feedback and bypass. Memory defaults to 40 s, rounded up by the
core. The plugin retains all 145 permanent ports.

Rec/Dub, Overdub, Undo, Mute and Play work through the command control ports.
The `commands` atom input also accepts the Milestone 1 commands for selected,
all, or explicit loop 1, including dedicated Record, Redo and Clear. Hold Undo
or Rec/Dub for more than a second to clear; a Rec/Dub double tap within 0.5 s
undoes twice. Use momentary host/stomp bindings so releases are delivered.

Loop 1 publishes real state, length, cycle and position. Loops 2 to 8 report
unallocated (-2); loop-count/selection changes and Next loop await Milestone 2.
Pause, sync, quantize, varispeed and clock outputs await Milestone 3. Bypass
uses a 10 ms fade, keeps loop time running, and restores loops that were not
already muted. Loop audio is lost when the plugin is recreated.

The upstream application's README is [README.sooperlooper.md](README.sooperlooper.md).
The LV2 build uses only the looping core and new glue; JACK, OSC, MIDI, the GUI,
RubberBand and libsamplerate application sources are not compiled.

- [docs/design.md](docs/design.md): what Loo-pi does, the spec.
- [docs/implementation-plan.md](docs/implementation-plan.md): ports, code layout, tests and milestones.
- [docs/peepo-page.md](docs/peepo-page.md): the Loo-pi page in Peepo, and the ports it uses.

## Build and install

On Linux (including Debian under WSL), install a C++17 compiler, Make,
pkg-config and LV2 headers: `build-essential pkg-config lv2-dev` on Debian.
No network access or autotools step is needed by the build.

```sh
make -j2
make install DESTDIR="$PWD/build/stage"
make install LV2DIR="$HOME/.lv2"
```

The bundle is `build/loo-pi.lv2/`, containing `loo-pi.so`, `manifest.ttl` and
`loo-pi.ttl`. Installation also copies `COPYING`. `PREFIX` defaults to
`/usr/local`; `LV2DIR` defaults to `$(PREFIX)/lib/lv2`, and `DESTDIR` stages the
installation without changing the live host. A native Pi build uses the same
commands; `OPT="-O2 -mcpu=cortex-a76"` selects the Pi 5 CPU.

Plugin URI: `https://github.com/ABustos2606/loo-pi#loopi`.
Hosts must provide `urid:map` and `work:schedule`. The worker is required now
so the host contract already supports adding loops in Milestone 2. The worker
interface is present; this version schedules no jobs. Initial memory is
allocated and touched during instantiation, outside the audio thread.

## Check

```sh
make test                         # also needs python3 and python3-rdflib
make test SAN=1                   # AddressSanitizer and UBSan, separate build
make test-timing                  # ten simulated minutes, one stereo loop
make lint                        # also needs lv2lint on PATH
make test-hosts                   # jalv, jackd, Ingen, python3-jack-client, numpy
```

`LV2LINT=/path/to/lv2lint` selects a local validator. `HOST_ARGS=--ingen=/path/to/ingen`
selects Ingen; its libraries must be on `LD_LIBRARY_PATH`. Peepo's WSL
environment can be sourced from its `tools/ingen-spike/env.sh` before running
the check. The host test starts a private dummy JACK server and engine socket;
logs stay in `build/host-smoke/`.

The port table lives in `tools/gen_ttl.py`; `make metadata` regenerates Turtle.
`tests/ttl_check.py` compares parsed Turtle against `lv2/ports.hpp` and checks
the original index/symbol list. Review changes carefully: saved patches depend
on these ports. The checked-in metadata lets a normal build run without Python.

Gain and feedback changes use fixed 10 ms linear ramps, independent of host
slice size; feedback moves in 32-sample steps. Threshold recording follows SooperLooper's positive left-channel
crossing; the right channel follows its sync at the same sample. Input gain
affects the recording send, while Dry level controls the untouched dry path.

See [Milestone 1 validation](docs/milestone-1-validation.md) for results and the
remaining physical Pi, release and Peepo integration checks. Desktop and
emulated timing do not establish real-time performance on the pedal.
