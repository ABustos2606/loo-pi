// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
// Milestone 4: loops saved and restored through LV2 state, with a host that
// gives the state its own directory (as lilv does for Ingen).
#include "fixture.hpp"
#include "../lv2/loop_state.hpp"
#include <lv2/state/state.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
using namespace loopi;
using namespace loopi::test;
namespace fs = std::filesystem;
constexpr float kPaused = SooperLooper::LooperStatePaused, kPlaying = SooperLooper::LooperStatePlaying;

namespace {
struct Saved {
    struct Value { uint32_t type, flags; std::vector<uint8_t> data; };
    std::map<uint32_t, Value> values;
    static LV2_State_Status store(LV2_State_Handle h, uint32_t key, const void* v, size_t size, uint32_t type, uint32_t flags) {
        auto& self = *static_cast<Saved*>(h);
        check(flags & LV2_STATE_IS_POD, "POD values");
        const auto* bytes = static_cast<const uint8_t*>(v);
        self.values[key] = {type, flags, std::vector<uint8_t>(bytes, bytes + size)};
        return LV2_STATE_SUCCESS;
    }
    static const void* retrieve(LV2_State_Handle h, uint32_t key, size_t* size, uint32_t* type, uint32_t* flags) {
        auto& self = *static_cast<Saved*>(h);
        const auto found = self.values.find(key);
        if (found == self.values.end()) { return nullptr; }
        *size = found->second.data.size(); *type = found->second.type; *flags = found->second.flags;
        return found->second.data.data();
    }
    bool has(const char* name) const { return values.count(map_uri(nullptr, (std::string("https://github.com/ABustos2606/loo-pi#") + name).c_str())); }
};

// mapPath, makePath and freePath over one directory, counting paths given out.
struct Dir {
    std::string path;
    int outstanding = 0;
    Dir() {
        char name[] = "/tmp/loopi-state-XXXXXX";
        check(mkdtemp(name), "temporary directory");
        path = name;
    }
    ~Dir() { std::error_code e; fs::remove_all(path, e); }
    char* give(const std::string& s) { ++outstanding; return strdup(s.c_str()); }
    static char* abstract(LV2_State_Map_Path_Handle h, const char* p) {
        auto& d = *static_cast<Dir*>(h);
        const std::string s = p, prefix = d.path + "/";
        return d.give(s.rfind(prefix, 0) == 0 ? s.substr(prefix.size()) : s);
    }
    static char* absolute(LV2_State_Map_Path_Handle h, const char* p) {
        auto& d = *static_cast<Dir*>(h);
        return d.give(p[0] == '/' ? std::string(p) : d.path + "/" + p);
    }
    static char* make(LV2_State_Make_Path_Handle h, const char* p) { auto& d = *static_cast<Dir*>(h); return d.give(d.path + "/" + p); }
    static void release(LV2_State_Free_Path_Handle h, char* p) { --static_cast<Dir*>(h)->outstanding; std::free(p); }
    LV2_State_Map_Path map{this, abstract, absolute};
    LV2_State_Make_Path maker{this, make};
    LV2_State_Free_Path freer{this, release};
    LV2_Feature map_f{LV2_STATE__mapPath, &map}, make_f{LV2_STATE__makePath, &maker}, free_f{LV2_STATE__freePath, &freer};
    const LV2_Feature* features[4]{&map_f, &make_f, &free_f, nullptr};
    bool exists(const char* name) const { return fs::exists(path + "/" + name); }
};

struct S : Fixture {
    uint32_t t = 0;
    float telemetry(uint32_t n, uint32_t field) { return h.buffers[kLoopBase + n * kLoopStride + field][0]; }
    double frames(uint32_t n, uint32_t field) { return std::round(double(telemetry(n, field)) * 48000); }
    const LV2_State_Interface& iface() {
        const auto* s = static_cast<const LV2_State_Interface*>(h.descriptor->extension_data(LV2_STATE__interface));
        check(s && s->save && s->restore, "state interface");
        return *s;
    }
    void save(Saved& saved, const LV2_Feature* const* features) {
        check(iface().save(h.handle, Saved::store, &saved, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, features) == LV2_STATE_SUCCESS, "save");
    }
    void restore(Saved& saved, const LV2_Feature* const* features) {
        check(iface().restore(h.handle, Saved::retrieve, &saved, 0, features) == LV2_STATE_SUCCESS, "restore");
    }
    // A signal no two frames of which are alike, so a misplaced frame shows.
    void signal(uint32_t frames) {
        while (frames) {
            const auto n = std::min(128U, frames);
            for (uint32_t i = 0; i < n; ++i, ++t) {
                h[Port::in_l][i] = .25f * std::sin(t * .0123f);
                h[Port::in_r][i] = .125f * std::cos(t * .0071f);
            }
            run(n);
            frames -= n;
        }
        h[Port::in_l].fill(0); h[Port::in_r].fill(0);
    }
    void rec(uint32_t target, uint32_t frames) { event(E::RECORD, target); signal(frames); event(E::RECORD, target); }
    std::vector<float> take(uint32_t frames) {
        std::vector<float> result;
        while (frames) {
            const auto n = std::min(128U, frames);
            run(n);
            for (uint32_t i = 0; i < n; ++i) { result.push_back(h[Port::out_l][i]); result.push_back(h[Port::out_r][i]); }
            frames -= n;
        }
        return result;
    }
};

// Loop 1 plain, loop 2 empty, loop 3 multiplied, loop 4 muted.
void make_loops(S& a) {
    a.ready(4);
    a.rec(2, 6000);
    a.rec(4, 4800);
    a.event(E::MULTIPLY, 4); a.advance(10000); a.event(E::MULTIPLY, 4); a.advance(10000);
    a.rec(5, 3000); a.event(E::MUTE, 5); a.advance(2000);
}

void round_trip() {
    Dir dir;
    Saved saved;
    std::vector<float> expected;
    double len[4], cyc[4];
    {
        S a; make_loops(a);
        check(a.frames(2, 12) < a.frames(2, 11), "loop 3 has several cycles");
        a.save(saved, dir.features);
        check(dir.outstanding == 0, "every path freed with freePath");
        check(dir.exists("loop1.wav") && !dir.exists("loop2.wav") && dir.exists("loop3.wav") && dir.exists("loop4.wav"), "a file per loop with audio");
        check(saved.has("loops") && saved.has("loop1") && !saved.has("loop2") && saved.has("loop3_cycle") && saved.has("loop4_memory"), "keys");
        for (uint32_t n = 0; n < 4; ++n) { len[n] = a.frames(n, 11); cyc[n] = a.frames(n, 12); }
        a.event(E::TRIGGER, 1);
        expected = a.take(30000);
    }
    S b;
    b.restore(saved, dir.features);
    check(dir.outstanding == 0, "restore frees its paths");
    // Ingen restores state at instantiation and sets the saved controls later.
    b.run(); b.advance(4800);
    check(b.state(0) == kPaused && b.state(2) == kPaused && b.state(3) == kPaused, "restored paused, held at 4 loops");
    check(b.state(1) == -2 && b.worker.nj == 1, "the empty loop is allocated by the worker");
    b.h[Port::loops][0] = 4; b.run(); b.worker.pump(b.h); b.run();
    check(b.state(1) == 0, "empty loop ready");
    for (uint32_t n = 0; n < 4; ++n) {
        check(b.frames(n, 11) == len[n], "length restored");
        if (n != 1) { check(b.frames(n, 12) == cyc[n], "cycle restored"); }
    }
    b.event(E::TRIGGER, 1);
    const auto got = b.take(30000);
    double worst = 0;
    for (size_t i = 4000; i < got.size(); ++i) { worst = std::max(worst, double(std::abs(got[i] - expected[i]))); }
    check(worst < 1e-6, "restored loops play the saved audio");
    // The hold ends at 4; Loops now acts as usual.
    b.h[Port::loops][0] = 2; b.advance(1000); b.worker.pump(b.h); b.run();
    check(b.state(2) == -2 && b.state(3) == -2 && b.state(0) == kPlaying, "Loops control acts after the hold");
    std::cout << "PASS round_trip: 4 loops, a multiplied cycle, a muted loop, back Paused, in phase, hold until Loops arrives\n";
}

void hold_change() {
    Dir dir; Saved saved;
    { S a; make_loops(a); a.save(saved, dir.features); }
    S b; b.restore(saved, dir.features);
    b.run(); b.h[Port::loops][0] = 3; b.advance(1000); b.worker.pump(b.h); b.run();
    check(b.state(3) == -2 && b.state(2) == kPaused, "a different Loops value ends the hold");
    std::cout << "PASS hold: a changed Loops control ends the hold\n";
}

void broken_files() {
    Dir dir; Saved saved;
    { S a; make_loops(a); a.save(saved, dir.features); }
    fs::remove(dir.path + "/loop3.wav");
    fs::resize_file(dir.path + "/loop4.wav", 30);
    S b; b.restore(saved, dir.features); b.h[Port::loops][0] = 4; b.run(); b.worker.pump(b.h); b.run();
    check(b.state(0) == kPaused && b.state(2) == 0 && b.state(3) == 0, "missing and short files give empty loops");
    // No paths at all (Ingen's duplicate): nothing is saved.
    Saved none;
    b.save(none, nullptr);
    check(none.values.empty(), "no files without makePath");
    // Restoring a state without loops changes nothing.
    b.restore(none, nullptr); b.run();
    check(b.state(0) == kPaused, "empty state leaves the loops");
    std::cout << "PASS broken files: missing, truncated, no makePath, empty state\n";
}

void pending() {
    Dir dir; Saved saved;
    { S a; make_loops(a); a.save(saved, dir.features); }
    S b; b.count(3);
    check(b.worker.nj == 2, "loops 2 and 3 allocating");
    b.restore(saved, dir.features);
    b.worker.pump(b.h); b.run();
    check(b.state(2) == kPaused && b.state(1) == 0, "restored loop kept over the late allocation");
    check(b.worker.nj == 1, "late allocation sent back to the worker");
    b.worker.pump(b.h); b.run();
    check(b.state(2) == kPaused, "orphan answer ignored");
    std::cout << "PASS pending: restore while the worker allocates\n";
}

void wav_formats() {
    Dir dir;
    const auto write = [&](const char* name, uint16_t format, uint16_t channels, uint16_t bits, const std::vector<uint8_t>& data) {
        std::vector<uint8_t> f;
        const auto u16 = [&](uint32_t v) { f.push_back(uint8_t(v)); f.push_back(uint8_t(v >> 8)); };
        const auto u32 = [&](uint32_t v) { u16(v & 0xffff); u16(v >> 16); };
        const auto tag = [&](const char* s) { f.insert(f.end(), s, s + 4); };
        tag("RIFF"); u32(uint32_t(36 + 10 + data.size())); tag("WAVE");
        tag("LIST"); u32(2); u16(0); // an odd chunk to skip
        tag("fmt "); u32(16); u16(format); u16(channels); u32(44100); u32(44100 * channels * bits / 8); u16(channels * bits / 8); u16(bits);
        tag("data"); u32(uint32_t(data.size())); f.insert(f.end(), data.begin(), data.end());
        FILE* file = std::fopen((dir.path + "/" + name).c_str(), "wb");
        std::fwrite(f.data(), 1, f.size(), file); std::fclose(file);
    };
    write("m16.wav", 1, 1, 16, {0x00, 0x40, 0x00, 0xc0});            // .5, -.5
    write("s24.wav", 1, 2, 24, {0, 0, 0x40, 0, 0, 0xe0});             // .5, -.25
    std::vector<float> l, r; uint32_t rate;
    check(read_wav((dir.path + "/m16.wav").c_str(), l, r, rate) && rate == 44100 && l.size() == 2, "16-bit mono");
    check(l[0] == .5f && r[0] == .5f && l[1] == -.5f && r[1] == -.5f, "mono to both channels");
    check(read_wav((dir.path + "/s24.wav").c_str(), l, r, rate) && l.size() == 1 && l[0] == .5f && r[0] == -.25f, "24-bit stereo");
    const std::vector<float> data{.1f, -.2f, .3f, -.4f};
    check(write_wav((dir.path + "/f.wav").c_str(), data, 48000), "write float");
    check(read_wav((dir.path + "/f.wav").c_str(), l, r, rate) && rate == 48000 && l == std::vector<float>{.1f, .3f} && r == std::vector<float>{-.2f, -.4f}, "float round trip");
    check(!read_wav((dir.path + "/none.wav").c_str(), l, r, rate), "missing file");
    std::cout << "PASS wav: float written and read, 16/24-bit PCM, mono, skipped chunks\n";
}

void concurrent_save() {
    // Saves on this thread while another runs the plugin, removing and
    // re-adding loops and overdubbing: no loop is freed while it is read.
    Dir dir;
    S f; f.ready(4); f.rec(2, 4000); f.rec(3, 5000); f.rec(4, 6000);
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> audio_heap{0};
    std::thread audio([&] {
        for (uint32_t i = 0; !stop.load(); ++i) {
            if (i % 40 == 0) { f.h[Port::loops][0] = i % 80 ? 1 : 4; }
            if (i % 25 == 0) { f.event(E::OVERDUB, 1); }
            f.signal(256);
            f.worker.pump(f.h);
        }
        audio_heap = heap_calls;
    });
    uint32_t saves = 0, saved_loops = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < until) {
        Saved saved;
        f.save(saved, dir.features);
        for (uint32_t n = 1; n <= 4; ++n) {
            const auto name = "loop" + std::to_string(n);
            if (!saved.has(name.c_str())) { continue; }
            std::vector<float> l, r; uint32_t rate;
            check(read_wav((dir.path + "/" + name + ".wav").c_str(), l, r, rate) && !l.empty(), "concurrent save readable");
            ++saved_loops;
        }
        ++saves;
    }
    stop = true;
    audio.join();
    check(audio_heap == 0, "no allocation in run() during saves");
    check(saves > 3 && saved_loops > 0, "saved while running");
    std::cout << "PASS concurrent save: " << saves << " saves, " << saved_loops << " loop files, while loops come and go\n";
}
} // namespace

int main() {
    try {
        wav_formats(); round_trip(); hold_change(); broken_files(); pending(); concurrent_save();
        check(heap_calls == 0, "no allocation/free during run or work_response");
        std::cout << "PASS state RT allocation guard\n";
    } catch (const std::exception& e) { std::cerr << "FAIL state: " << e.what() << '\n'; return 1; }
}
