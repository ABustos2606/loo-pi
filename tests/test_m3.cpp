// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
// Milestone 3: the remaining commands, quantize, sync to Tempo and to a loop,
// Rate and the clock outputs, through the plugin and its worker.
#include "fixture.hpp"
#include <chrono>
#include <cmath>
#include <iostream>
#include <vector>
using namespace loopi;
using namespace loopi::test;

namespace {
struct M3 : Fixture {
    M3() { h[Port::bpm][0] = 120; h[Port::eighths][0] = 16; }
    float out(uint32_t i = 0) { return h[Port::out_l][i]; }
    float telemetry(uint32_t n, uint32_t field) { return h.buffers[kLoopBase + n * kLoopStride + field][0]; }
    double frames(uint32_t n, uint32_t field) { return std::round(double(telemetry(n, field)) * 48000); }
    double len(uint32_t n = 0) { return frames(n, 11); }
    double cyc(uint32_t n = 0) { return frames(n, 12); }
    double pos(uint32_t n = 0) { return frames(n, 13); }
    float waiting(uint32_t n = 0) { return telemetry(n, 10); }
    void input(float v) { h[Port::in_l].fill(v); h[Port::in_r].fill(-v); }
    // A free loop of `frames` + 1 frames (the closing command takes one).
    void rec(uint32_t frames, float level, uint32_t target = 0) {
        input(level); event(E::RECORD, target); advance(frames); event(E::RECORD, target); input(0);
    }
    // Runs `frames` frames and returns the left output, one value per frame.
    std::vector<float> take(uint32_t frames) {
        std::vector<float> result;
        while (frames) {
            const auto n = std::min(128U, frames);
            run(n);
            result.insert(result.end(), h[Port::out_l].begin(), h[Port::out_l].begin() + n);
            frames -= n;
        }
        return result;
    }
};
void near(double a, double b, double tolerance, const char* message) { check(std::abs(a - b) <= tolerance, message); }

void commands_m3()
{
    { // Multiply: a whole number of cycles, cycle length kept.
        M3 f; f.rec(4800, .125f); f.advance(100);
        f.event(E::MULTIPLY); f.advance(2000); check(f.state(0) == 6, "multiplying");
        f.advance(10000); f.event(E::MULTIPLY); f.advance(2000);
        check(f.state(0) == 4 && f.cyc() == 4801 && f.len() == 3 * 4801, "multiply three cycles");
    }
    { // Insert: adds whole cycles of the new (silent) input.
        M3 f; f.rec(4800, .125f); f.advance(100);
        f.event(E::INSERT); f.advance(7000); f.event(E::INSERT);
        check(f.state(0) == 7, "inserting until the cycle ends");
        f.advance(1200); check(f.out() == 0, "inserted cycle is the silent input");
        f.advance(4800); check(f.state(0) == 4 && f.len() == 3 * 4801 && f.cyc() == 4801, "insert whole cycles");
    }
    { // Replace: the old loop is not heard; Substitute: it is.
        for (auto command : {E::REPLACE, E::SUBSTITUTE}) {
            M3 f; f.rec(4800, .125f); f.advance(4700);
            f.input(.25f); f.event(command); f.advance(100);
            check(f.state(0) == (command == E::REPLACE ? 8 : 13), "replace/substitute state");
            near(f.out(), command == E::REPLACE ? 0 : .125, 1e-6, "old loop heard only while substituting");
            f.advance(4800); f.event(command); f.input(0); f.advance(200);
            check(f.state(0) == 4, "back to play");
            near(f.out(), .25, 1e-6, "loop replaced by the input");
        }
    }
    { // Reverse: the position runs backwards; a long hold reverses back on release.
        M3 f; f.rec(4800, .125f); f.advance(1000);
        f.event(E::REVERSE); const auto a = f.pos(); f.advance(100);
        check(f.pos() == a - 100 && f.state(0) == 4, "reverse runs backwards");
        f.event(E::REVERSE); f.advance(100); const auto b = f.pos(); f.advance(100);
        check(f.pos() == b + 100, "reverse again runs forwards");
        f.event(E::REVERSE); f.advance(48001); f.event(E::REVERSE, 0, false); const auto c = f.pos(); f.advance(100);
        check(f.pos() == c + 100, "long Reverse release acts although playing (Looper::do_event)");
    }
    { // Once: plays to the end, then mutes.
        M3 f; f.rec(4800, .125f); f.advance(1000);
        f.event(E::ONESHOT); f.advance(100); check(f.state(0) == 12 && f.out() == .125f, "once plays from the start");
        f.advance(4800); check(f.state(0) == 10 && f.out() == 0, "once mutes at the end");
    }
    { // Pause: stops in place, silent; again carries on from the same place.
        M3 f; f.rec(4800, .125f); f.advance(1000);
        f.event(E::PAUSE); const auto at = f.pos(); f.advance(1000);
        check(f.state(0) == 14 && f.pos() == at && f.out() == 0, "pause holds position, silent");
        f.event(E::PAUSE); f.advance(100); check(f.state(0) == 4 && f.pos() == at + 101, "pause again carries on");
        f.h[Port::pause][0] = 1; f.run(1); f.h[Port::pause][0] = 0; f.run(1);
        check(f.state(0) == 14, "pause control press");
        f.h[Port::pause][0] = 1; f.run(1); f.h[Port::pause][0] = 0; f.run(1);
        check(f.state(0) == 4, "pause control again");
    }
    { // Delay: the time between two presses becomes a delay line.
        M3 f;
        f.event(E::DELAY); f.advance(2399); f.event(E::DELAY);
        check(f.state(0) == 9 && f.len() == 2400, "delay length is the time between presses");
        f.advance(2400); // the first pass clears the line
        f.h[Port::in_l][0] = .5f; f.h[Port::in_r][0] = -.5f; f.run(1); f.input(0);
        const auto echo = f.take(2400);
        check(echo[2399] == .5f && echo[2398] == 0, "echo exactly one delay time after the input");
    }
    std::cout << "PASS commands_m3: Multiply, Insert, Replace, Substitute, Reverse (long release), Once, Pause (control), Delay\n";
}

void quantize()
{
    M3 f; f.h[Port::quantize][0] = 1; f.h[Port::dub_quant][0] = 1;
    f.rec(4800, .125f); f.advance(1000);
    f.input(.25f); f.event(E::OVERDUB);
    check(f.state(0) == 4 && f.waiting() == 1 && f.telemetry(0, 9) == 5, "quantized overdub waits for the cycle");
    uint32_t waited = 0;
    while (f.state(0) != 5 && waited < 5000) { f.run(1); ++waited; }
    check(f.state(0) == 5 && f.waiting() == 0 && f.pos() <= 1, "overdub starts on the cycle boundary");
    f.input(0); f.event(E::OVERDUB); f.advance(100);
    f.event(E::OVERDUB); check(f.waiting() == 1, "second overdub waits");
    f.event(E::UNDO); check(f.waiting() == 0, "Undo cancels a waiting command");
    f.advance(6000); check(f.state(0) == 4, "cancelled command never starts");
    f.h[Port::mute_quant][0] = 1; f.event(E::MUTE); check(f.state(0) == 4 && f.waiting() == 1, "quantized mute waits");
    f.advance(5000); check(f.state(0) == 10, "quantized mute on the boundary");
    std::cout << "PASS quantize: waits for the cycle with waiting = 1, starts on the boundary, Undo cancels, Mute quantized\n";
}

void tempo()
{
    M3 f; f.h[Port::quantize][0] = 1; f.h[Port::sync_to][0] = 1; f.h[Port::l1_sync][0] = 1;
    f.input(.125f); f.advance(3000); f.event(E::RECORD);
    check(f.state(0) == 1 && f.waiting() == 1, "synced record waits for the tempo cycle");
    f.advance(192000 + 150000); f.event(E::RECORD);
    check(f.state(0) == 3, "synced record waits to stop");
    f.advance(192000); f.input(0);
    check(f.state(0) == 4 && f.len() == 192000 && f.cyc() == 192000, "120 BPM, 16 eighths: exactly 4 s");
    std::cout << "PASS tempo: Sync to Tempo at 120 BPM, 16 eighths, makes a loop of exactly 192000 frames\n";
}

void sync_loop()
{
    M3 f; f.ready(2);
    f.h[Port::quantize][0] = 1; f.h[Port::sync_to][0] = 2; f.h[Port::l2_sync][0] = 1;
    f.rec(48000, .125f); f.advance(1000);
    check(f.len(0) == 48001, "source loop");
    f.input(.25f); f.event(E::RECORD, 3); f.advance(2 * 48000 + 7000); f.event(E::RECORD, 3); f.input(0);
    f.advance(2 * 48000);
    check(f.state(1) == 4 && f.len(1) == 2 * 48001, "synced loop records whole cycles of loop 1");
    const auto phase = [&] { return std::fmod(f.pos(1), 48001.0) == f.pos(0); };
    check(phase(), "in phase");
    for (uint32_t n = 0; n < 600 * 48000 / 1024; ++n) { f.run(1024); }
    check(phase() && f.len(1) == 2 * 48001, "still in phase after 10 simulated minutes");
    std::cout << "PASS sync_loop: synced to loop 1, whole cycles, in phase for 10 minutes\n";
}

void rate()
{
    M3 f;
    // 48 periods of a 480 Hz sine (100 frames each), stereo.
    f.event(E::RECORD);
    for (uint32_t block = 0; block < 4800 / 100; ++block) {
        for (uint32_t i = 0; i < 100; ++i) {
            const auto v = float(.25 * std::sin(2 * M_PI * i / 100));
            f.h[Port::in_l][i] = v; f.h[Port::in_r][i] = -v;
        }
        f.run(100);
    }
    f.event(E::RECORD); f.input(0); f.advance(4800);
    const auto rises = [](const std::vector<float>& v) {
        uint32_t n = 0;
        for (size_t i = 1; i < v.size(); ++i) { n += v[i - 1] < 0 && v[i] >= 0; }
        return n;
    };
    const auto one = f.take(9600);
    near(rises(one), 96, 1, "rate 1: 480 Hz");
    f.h[Port::l1_rate][0] = .5f; f.run(1);
    const auto a = f.pos(); const auto half = f.take(9600);
    near(rises(half), 48, 1, "rate 0.5: an octave down");
    near(std::fmod(f.pos() - a + 4801, 4801.0), 4800, 2, "rate 0.5: half speed");
    f.h[Port::l1_rate][0] = 2; f.run(1);
    near(rises(f.take(9600)), 192, 1, "rate 2: an octave up");
    f.h[Port::l1_rate][0] = 1; f.run(1);
    const auto back = f.take(9600);
    near(rises(back), 96, 1, "rate 1 again");
    float peak = 0; for (auto v : back) { peak = std::max(peak, std::abs(v)); }
    near(peak, .25, 1e-3, "rate 1 plays the recording unchanged");
    std::cout << "PASS rate: 0.5 is an octave down at half speed, 2 an octave up, 1 unchanged\n";
}

// Rising edges of a clock output over `frames` frames, as absolute frame numbers.
std::vector<uint64_t> edges(M3& f, Port port, uint32_t frames, uint64_t& at, uint32_t& width)
{
    std::vector<uint64_t> result;
    float last = f.h[port][0]; uint32_t high = last == 1; // the previous run was one frame
    for (uint32_t done = 0; done < frames;) {
        const auto n = std::min(128U, frames - done);
        f.run(n);
        for (uint32_t i = 0; i < n; ++i) {
            const float v = f.h[port][i];
            if (v == 1 && last == 0) { result.push_back(at + i); high = 0; }
            if (v == 1) { width = std::max(width, ++high); }
            last = v;
        }
        at += n; done += n;
    }
    return result;
}

void clocks()
{
    M3 f; f.input(.125f); f.event(E::RECORD); f.advance(4799);
    uint64_t at = 0; uint32_t width = 0;
    f.event(E::RECORD); f.input(0);
    check(f.h[Port::loop_clock][0] == 1 && f.h[Port::cycle_clock][0] == 1 && f.len() == 4800,
          "closing a recording starts the loop: both clocks trigger");
    auto loops = edges(f, Port::loop_clock, 48010, at, width);
    check(loops.size() == 10 && loops[0] == 4799 && width == 48, "loop clock: a 1 ms trigger at every loop start");
    for (size_t i = 1; i < loops.size(); ++i) { check(loops[i] - loops[i - 1] == 4800, "loop clock period"); }
    f.advance(2400); at += 2400; f.event(E::REVERSE); ++at; width = 0;
    loops = edges(f, Port::loop_clock, 48000, at, width);
    auto cycles = edges(f, Port::cycle_clock, 48000, at, width);
    check(loops.size() == 10 && cycles.size() == 10, "reversed: one loop and cycle trigger per pass");
    for (size_t i = 1; i < loops.size(); ++i) { check(loops[i] - loops[i - 1] == 4800, "reversed loop clock period"); }
    M3 g; g.rec(4800, .125f); g.event(E::MULTIPLY); g.advance(10000); g.event(E::MULTIPLY); g.advance(10000);
    at = 0; width = 0;
    cycles = edges(g, Port::cycle_clock, 3 * 14403, at, width);
    check(cycles.size() == 9, "cycle clock: three per loop after Multiply");
    for (size_t i = 1; i < cycles.size(); ++i) { check(cycles[i] - cycles[i - 1] == 4801, "cycle clock period"); }
    g.h[Port::l1_rate][0] = .5f; g.run(1); at = 0; width = 0;
    cycles = edges(g, Port::cycle_clock, 4 * 9602, at, width);
    for (size_t i = 1; i < cycles.size(); ++i) { near(double(cycles[i] - cycles[i - 1]), 9602, 1, "cycle clock at rate 0.5"); }
    std::cout << "PASS clocks: 1 ms triggers on loop and cycle starts, from the close, Reverse, Multiply cycles, Rate\n";
}

void slicing()
{
    const auto render = [](int mode) { // 0 blocks, 1 random slices, 2 single frames
        const bool random = mode == 1;
        M3 f; f.ready(3);
        f.h[Port::quantize][0] = 1; f.h[Port::sync_to][0] = 2; f.h[Port::l2_sync][0] = 1; f.h[Port::l3_sync][0] = 1;
        f.h[Port::bpm][0] = 133;
        f.input(.03125f);
        struct Action { uint32_t frame, cmd, target; int rate; };
        const Action actions[]{{0, E::RECORD, 2, 0}, {3000, E::RECORD, 2, 0}, {3500, E::RECORD, 3, 0},
            {9000, E::RECORD, 3, 0}, {9100, E::OVERDUB, 4, 0}, {12000, E::OVERDUB, 4, 0}, {12500, 0, 0, 3},
            {13000, E::MULTIPLY, 3, 0}, {19000, E::MULTIPLY, 3, 0}, {19500, 0, 0, 1}, {20000, E::REVERSE, 2, 0},
            {22000, E::PAUSE, 4, 0}, {23000, E::PAUSE, 4, 0}, {24000, E::DELAY, 4, 0}, {25000, E::DELAY, 4, 0}};
        uint32_t frame = 0, rng = 23;
        std::vector<float> result; result.reserve(160000);
        const auto keep = [&](uint32_t n) {
            for (uint32_t i = 0; i < n; ++i) {
                for (auto p : {Port::out_l, Port::out_r, Port::cycle_clock, Port::loop_clock}) { result.push_back(f.h[p][i]); }
            }
        };
        const auto advance = [&](uint32_t end) {
            while (frame < end) {
                rng = rng * 1664525 + 1013904223;
                const auto n = std::min(end - frame, mode == 2 ? 1U : random ? 1 + rng % 127 : 128U);
                f.run(n); keep(n); frame += n;
            }
        };
        for (const auto& a : actions) {
            advance(a.frame);
            if (a.rate) {
                f.h[Port::l2_rate][0] = a.rate == 3 ? .75f : 1; f.h[Port::l3_rate][0] = a.rate == 3 ? 1.5f : 1;
                f.run(1);
            } else { f.event(a.cmd, a.target); }
            keep(1); ++frame;
        }
        f.h[Port::sync_to][0] = 1; // Tempo
        advance(36000);
        return result;
    };
    // 128-frame blocks, random slices and one frame at a time.
    const auto blocks = render(0);
    check(blocks == render(1) && blocks == render(2), "host slicing changes audio or clocks");
    std::cout << "PASS slicing: exact output and clocks (blocks, random, single frames) with sync to a loop and Tempo, "
                 "Rate, Multiply, quantized Reverse, Pause, Delay\n";
}

// Upstream's beginOverdub "overdub using self" (a loop as long as its memory)
// left a chunk that is its own source, and fillLoops descended it forever.
void memory_full_multiply()
{
    M3 f; f.ready(1); f.h[Port::quantize][0] = 0;
    f.rec(48000, .25f);
    // Memory is 10 s (the minimum); 30 s of Multiply runs into its end.
    f.input(.125f); f.event(E::MULTIPLY); f.advance(48000 * 30);
    f.event(E::OVERDUB); f.advance(48000 * 3);
    f.event(E::OVERDUB); f.input(0); f.advance(48000);
    check(f.state(0) == SooperLooper::LooperStatePlaying, "Overdub after a memory-full Multiply plays");
    std::cout << "PASS memory_full_multiply: Overdub after Multiply fills memory, no endless fill\n";
}

void timing()
{
    M3 f; f.ready(8); f.h[Port::quantize][0] = 1; f.h[Port::sync_to][0] = 2;
    for (uint32_t n = 0; n < 8; ++n) {
        f.h.buffers[kLoopBase + n * kLoopStride + 4][0] = n ? 1 : 0;
        f.h.buffers[kLoopBase + n * kLoopStride + 7][0] = n % 2 ? .5f : 1.5f;
    }
    f.rec(48000, .03125f, 1);
    std::vector<double> times; times.reserve(225000);
    for (uint32_t n = 0; n < 225000; ++n) {
        if (n % 10000 == 0) { f.event(n % 20000 ? E::MULTIPLY : E::OVERDUB, 1); }
        const auto start = std::chrono::steady_clock::now(); f.run(128);
        times.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());    }
    std::sort(times.begin(), times.end());
    std::cout << "PASS timing: 600 simulated seconds, 8 stereo loops synced to loop 1, Rate 0.5/1.5; p99.9 "
              << times[times.size() * 999 / 1000] << " us, max " << times.back() << " us\n";
}
} // namespace

int main(int argc, char**)
{
    try {
        commands_m3(); quantize(); tempo(); sync_loop(); rate(); clocks(); slicing(); memory_full_multiply();
        check(heap_calls == 0, "no allocation/free during run or work_response");
        if (argc > 1) { timing(); }
        check(heap_calls == 0, "no allocation/free during timing");
        std::cout << "PASS M3 RT allocation guard\n";
    } catch (const std::exception& e) { std::cerr << "FAIL M3: " << e.what() << '\n'; return 1; }
}
