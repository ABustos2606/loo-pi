// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#include "host.hpp"
#include <lv2/atom/atom.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/options/options.h>
#include "../src/event.hpp"
#include <dlfcn.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

using namespace loopi;
using namespace loopi::test;
void near(float a, float b, const char* message)
{
    if (!std::isfinite(a) || std::abs(a - b) >= 0.00002f) {
        std::cerr << message << ": " << a << " expected " << b << '\n';
        throw std::runtime_error(message);
    }
}

void advance(Host& host, uint32_t count, float left = 0, float right = 0)
{
    host[Port::in_l].fill(left); host[Port::in_r].fill(right);
    while (count) { auto n = std::min(128U, count); host.run(n); count -= n; }
}
void press(Host& host, Port p)
{ host[p][0] = 1; host.run(1); }
void release(Host& host, Port p)
{ host[p][0] = 0; host.run(1); }

void controls(const LV2_Descriptor* d)
{
    Host host(d);
    host[Port::safety_fb][0] = 0;
    host[Port::fade][0] = 0;
    host[Port::dry_level][0] = 0;
    host[Port::in_l].fill(0.125f); host[Port::in_r].fill(-0.25f);
    host[Port::rec_dub][0] = 1;
    host.run(0); check(host[Port::l1_state][0] == 0, "zero run does not consume Record edge");
    host.run(1); release(host, Port::rec_dub);
    advance(host, 35998, 0.125f, -0.25f);
    near(host[Port::l1_len][0], 0.75f, "control records 0.75 seconds");
    press(host, Port::rec_dub); release(host, Port::rec_dub);
    advance(host, 4096);
    check(host[Port::l1_state][0] == 4, "Rec/Dub closes into Play");
    near(host[Port::out_l][63], 0.125f, "LV2 playback left");
    near(host[Port::out_r][63], -0.25f, "LV2 playback right");
    host[Port::l1_in_gain][0] = 0.5f;
    advance(host, 1024);
    press(host, Port::overdub); release(host, Port::overdub);
    advance(host, 40000, 0.125f, -0.25f);
    press(host, Port::overdub); release(host, Port::overdub);
    advance(host, 40000);
    // Some samples receive two passes; Undo must restore the uniform base.
    press(host, Port::undo); release(host, Port::undo);
    advance(host, 36000);
    near(host[Port::out_l][63], 0.125f, "LV2 Undo restores base");
    press(host, Port::trigger); release(host, Port::trigger);
    advance(host, 126);
    near(host[Port::l1_pos][0], 128.0f / 48000, "LV2 Trigger time");
    host[Port::enabled][0] = 0;
    advance(host, 2048);
    near(host[Port::out_l][63], 0, "LV2 bypass mutes loop");
    host[Port::enabled][0] = 1;
    advance(host, 2048);
    near(host[Port::out_l][63], 0.125f, "LV2 bypass restores loop");
    press(host, Port::mute); release(host, Port::mute);
    advance(host, 2048);
    host[Port::enabled][0] = 0; advance(host, 1024);
    host[Port::enabled][0] = 1; advance(host, 1024);
    check(host[Port::l1_state][0] == 10, "LV2 preserves pre-bypass mute");
    press(host, Port::mute); release(host, Port::mute);
    advance(host, 2048);
    press(host, Port::undo);
    advance(host, 48001);
    release(host, Port::undo);
    advance(host, 2048);
    near(host[Port::l1_len][0], 0, "LV2 long Undo clears");
    std::cout << "PASS LV2 commands: Record/RecDub, Overdub, Undo, Trigger, Mute, long Clear, bypass\n";
}

void nonfinite(const LV2_Descriptor* d)
{
    Host host(d);
    for (auto bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                     -std::numeric_limits<float>::infinity()}) {
        host[Port::l1_feedback][0] = host[Port::l1_in_gain][0] = host[Port::fade][0] = host[Port::dry_level][0] = bad;
        host[Port::in_l].fill(bad); host[Port::in_r].fill(0.125f);
        host[Port::rec_dub][0] = 1; host.run(128);
        for (uint32_t i = 0; i < 128; ++i) {
            check(std::isfinite(host[Port::out_l][i]) && std::isfinite(host[Port::out_r][i]), "nonfinite guards");
        }
        host[Port::rec_dub][0] = 0; host.run(128);
    }
    // Missing input is real silence; do not infer routing from connections.
    d->connect_port(host.handle, index(Port::in_l), nullptr);
    d->connect_port(host.handle, index(Port::in_r), nullptr);
    host[Port::l1_level][0] = 0;
    advance(host, 2048);
    near(host[Port::out_l][63], 0, "null input silence");
    std::cout << "PASS nonfinite: NaN/Inf audio and controls, null inputs\n";

    Host overflow(d);
    overflow[Port::dry_level][0] = 0;
    overflow[Port::fade][0] = 0;
    overflow[Port::safety_fb][0] = 0;
    const auto huge = std::numeric_limits<float>::max() * 0.75f;
    overflow[Port::in_l].fill(huge); overflow[Port::in_r].fill(-huge);
    press(overflow, Port::rec_dub); release(overflow, Port::rec_dub);
    advance(overflow, 36000, huge, -huge);
    press(overflow, Port::rec_dub); release(overflow, Port::rec_dub);
    advance(overflow, 4800);
    press(overflow, Port::overdub); release(overflow, Port::overdub);
    overflow[Port::in_l].fill(huge); overflow[Port::in_r].fill(-huge);
    for (uint32_t i = 0; i < 1000; ++i) {
        overflow.run(128);
        for (uint32_t n = 0; n < 128; ++n) {
            check(std::isfinite(overflow[Port::out_l][n]) && std::isfinite(overflow[Port::out_r][n]), "finite overflow recovery");
        }
    }
    check(overflow[Port::l1_len][0] == 0, "poisoned loop is logically cleared");
    std::cout << "PASS nonfinite: finite-input arithmetic overflow silences slice and clears loop\n";
}

void options(const LV2_Descriptor* d)
{
    for (int32_t maximum : {1, 37, 2048, 32768}) {
        LV2_Options_Option options[]{
            {LV2_OPTIONS_INSTANCE, 0, map_uri(nullptr, LV2_BUF_SIZE__maxBlockLength), sizeof(maximum),
             map_uri(nullptr, LV2_ATOM__Int), &maximum}, {}};
        LV2_Feature option_feature{LV2_OPTIONS__options, options};
        const LV2_Feature* list[]{&map_feature, &worker_feature, &option_feature, nullptr};
        Host host(d, list);
        // Deliberately exceed the advertised maximum and internal chunk size.
        std::vector<float> left(40001, 0.125f), right(40001, -0.25f), out_l(40001, 99), out_r(40001, 99);
        d->connect_port(host.handle, index(Port::in_l), left.data());
        d->connect_port(host.handle, index(Port::in_r), right.data());
        d->connect_port(host.handle, index(Port::out_l), out_l.data());
        d->connect_port(host.handle, index(Port::out_r), out_r.data());
        d->connect_port(host.handle, index(Port::cycle_clock), nullptr);
        d->connect_port(host.handle, index(Port::loop_clock), nullptr);
        host.run(40000);
        check(out_l[39999] == 0.125f && out_r[39999] == -0.25f && out_l[40000] == 99 && out_r[40000] == 99, "oversize run bounds");
    }
    std::cout << "PASS options: 1/37/2048/32768 frame maxima, oversize 40000-frame runs\n";
}

struct Events {
    std::array<uint64_t, 128> storage{};
    LV2_Atom_Sequence* sequence = reinterpret_cast<LV2_Atom_Sequence*>(storage.data());
    Events()
    {
        sequence->atom.type = map_uri(nullptr, LV2_ATOM__Sequence);
        sequence->atom.size = sizeof(sequence->body);
    }
    void add(int64_t frame, int32_t command, bool press = true, int32_t target = 0)
    {
        LV2_Atom_Event event{};
        event.time.frames = frame;
        event.body.type = map_uri(nullptr, LV2_ATOM__Int);
        event.body.size = sizeof(int32_t);
        auto* destination = reinterpret_cast<uint8_t*>(sequence) + sizeof(LV2_Atom) + sequence->atom.size;
        std::memcpy(destination, &event, sizeof(event));
        const int32_t value = target * 256 + command * 2 + int(press);
        std::memcpy(destination + sizeof(event), &value, sizeof(value));
        sequence->atom.size += sizeof(event) + 8;
    }
    void send(Host& host, uint32_t frames = 128)
    {
        host.descriptor->connect_port(host.handle, index(Port::commands), sequence);
        host.run(frames);
        host.descriptor->connect_port(host.handle, index(Port::commands), nullptr);
    }
};

void atoms(const LV2_Descriptor* d)
{
    using E = SooperLooper::Event;
    Host host(d);
    host[Port::fade][0] = host[Port::dry_level][0] = host[Port::safety_fb][0] = 0;
    host[Port::in_l].fill(0.125f); host[Port::in_r].fill(-0.25f);
    Events record;
    record.add(10, E::RECORD); record.add(11, E::RECORD, false);
    record.add(100, E::RECORD); record.add(101, E::RECORD, false);
    record.send(host);
    near(host[Port::l1_len][0], 90.0f / 48000, "two atom Record commands at exact timestamps");
    advance(host, 1024);
    Events dub; dub.add(0, E::OVERDUB, true, 2); dub.add(1, E::OVERDUB, false, 2);
    dub.send(host); advance(host, 900, 0.0625f, -0.125f); // Ten whole 90-frame cycles.
    Events stop; stop.add(0, E::OVERDUB); stop.send(host); advance(host, 1024);
    const auto dubbed = host[Port::out_l][63];
    check(dubbed > 0.125f, "explicit loop 1 atom Overdub");
    Events undo; undo.add(0, E::UNDO, true, 1); undo.send(host); advance(host, 1024);
    near(host[Port::out_l][63], 0.125f, "atom Undo with all target in one-loop plugin");
    Events redo; redo.add(0, E::REDO); redo.send(host); advance(host, 1024);
    near(host[Port::out_l][63], dubbed, "atom Redo");
    Events clear; clear.add(0, E::UNDO_ALL); clear.send(host); advance(host, 1024);
    check(host[Port::l1_len][0] == 0, "atom Clear");

    Events malformed;
    malformed.add(-1, E::RECORD); // Negative timestamp.
    malformed.add(128, E::RECORD); // Outside this slice.
    malformed.add(0, E::RECORD, true, 3); // Unallocated loop 2.
    malformed.add(0, 127); // Unknown command.
    malformed.add(0, E::SCRATCH); // Not supported: no scratch position control.
    malformed.send(host);
    check(host[Port::l1_len][0] == 0 && host[Port::l1_state][0] == 0, "invalid/unsupported atoms ignored");
    Events truncated;
    truncated.add(0, E::RECORD);
    --truncated.sequence->atom.size;
    truncated.send(host);
    check(host[Port::l1_state][0] == 0, "truncated atom ignored");
    Events wrong_type;
    wrong_type.add(0, E::RECORD);
    wrong_type.sequence->atom.type = map_uri(nullptr, LV2_ATOM__Float);
    wrong_type.send(host);
    check(host[Port::l1_state][0] == 0, "wrong sequence type ignored");
    std::cout << "PASS atoms: timed Record, loop 1/all targets, Overdub, Undo, Redo, Clear, malformed/reserved input\n";
}

int main(int argc, char** argv)
{
    try {
        check(argc == 2, "usage: test_plugin plugin.so");
        auto* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        if (!library) { throw std::runtime_error(dlerror()); }
        auto get = reinterpret_cast<const LV2_Descriptor* (*)(uint32_t)>(dlsym(library, "lv2_descriptor"));
        check(get && get(0), "descriptor");
        controls(get(0)); nonfinite(get(0)); options(get(0)); atoms(get(0));
        dlclose(library);
    } catch (const std::exception& error) {
        std::cerr << "FAIL plugin: " << error.what() << '\n'; return 1;
    }
}
