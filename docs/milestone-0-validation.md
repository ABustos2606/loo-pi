<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
<!-- Copyright (c) 2026 Alejandro Bustos -->
# Milestone 0 validation

Recorded 19 October 2026. The scaffold is implemented. It passes dry stereo
audio at Dry level and allocates one idle stereo loop. Recording, loop mixing,
command processing, additional loops, sync and varispeed remain later work.

| Check | Result |
| --- | --- |
| x86-64 `make -j2 test`, Debian under WSL | PASS; all 145 ports, permanent ABI, scalar defaults/ranges/properties, descriptor and lifecycle checks |
| `make -j2 test SAN=1` | PASS; AddressSanitizer and UBSan, including concurrent instance creation and repeated cleanup |
| arm64 `make -j2 test BUILD_DIR=build/arm64 OPT="-O2 -mcpu=cortex-a76"` | PASS; actual AArch64 library and executable under QEMU in the existing Debian 12 root |
| `make install DESTDIR="$PWD/build/stage"` | PASS; library, both Turtle files and COPYING under `usr/local/lib/lv2/loo-pi.lv2/` |
| `lv2lint` 0.17.151 | No errors; author-email warning described below |
| jalv, private dummy JACK at 48 kHz | PASS; discovery, required features and exact stereo dry levels 0.125 / -0.25 |
| Peepo's patched Ingen, one and four processing threads | PASS; complete port discovery, stereo dry audio, Dry level changes and enabled-port dry passthrough |

Offline audio checks exercise variable and random slices, zero-frame runs,
buffer boundary guards, in-place processing, three dry levels, nonfinite input,
null port connections and unknown indices. Loop 1 reports state 0, remaining
loops report -2, clocks stay silent, and selected/solo/free-memory telemetry is
initialized even on zero-frame calls. Hosts lacking either `urid:map` or
`work:schedule` are rejected. The worker interface is discoverable; no jobs are
scheduled in Milestone 0. Command and future setting ports remain inert.

The normal bundle builds only `src/plugin.cc` and `lv2/lv2_entry.cpp`; it does
not link the application dependencies. The export map exposes only
`lv2_descriptor`. The Makefile suppresses old unused-variable, unused-parameter,
signedness and fallthrough warnings for the core alone. New glue and test code
retain `-Wall -Wextra`; IEEE finite checks stay enabled even if OPT contains
fast-math flags.

## Allocation and core changes

Initial stereo memory is allocated and touched during LV2 instantiation.
Both channels use an explicit 40 s request, which rounds to 2,097,152 samples
per channel at 48 kHz (about 16 MiB stereo, 43.69 s). Nothing is allocated or
freed in `run()`. Each instance owns its core controls and handles and does not
use the core's mutable global LADSPA descriptor.

Two planned Milestone 1 changes were brought forward: the environment-based
memory-size setting was replaced with `sl_instantiate(rate, seconds)`, and the
iostream include is conditional on `LOOPDEBUG`. A process-wide setter would
race between instances or workers, so the plan now specifies an explicit
argument. The core's allocation path also checks failure of its input-latency
buffer, frees the instance after partial failure, and releases that buffer on
normal cleanup. These are lifecycle fixes; the core audio algorithm is unchanged.
The scalar initialization follows `Looper::initialize`, with attribution in
the glue. The full stereo command layer is still Milestone 1 work.

The `loop_secs` control cannot be read until the host connects ports, after
instantiation. In this scaffold it remains reserved and the first loop always
gets the default 40 s; later it controls newly added loops as specified.

## Reproduction and limits

Run `make test`, `make test SAN=1`, staged `make install`, `make lint` and
`make test-hosts`. The test dependencies and Ingen environment are described
in the README. Host logs are retained in `build/host-smoke/`, including JACK,
jalv, both Ingen instances and their protocol transcripts. The test uses a
private named JACK server and private sockets and does not stop an existing
Peepo engine or JACK server.

For this check, `LV2LINT` selected Discipline's existing local validator at
`/mnt/c/LV2-own-plugins/discipline/build/lint-tools/bin/lv2lint`. That build
disables online, ELF and X11 checks; dynamic exports and library dependencies
were inspected separately. Its one remaining warning is **Plugin Author
Email**: no `foaf:mbox` is supplied. The metadata includes the author's name
and GitHub homepage; no email address has been invented or published.

The arm64 build used a temporary workspace bind mount in the existing
`/opt/peepo-arm64` root, removed after the check. This is emulated functional
validation, not a native Pi performance measurement. WSL's non-realtime JACK
server and Python probe may report memory-lock/scheduling limitations; no
physical xrun-performance claim follows from these tests.

The required physical Pi check and its Peepo evidence are outstanding. The
milestone checkbox remains open and `v0.0.1` has not been created: create that
release tag on the reviewed commit after the hardware gate is satisfied.
