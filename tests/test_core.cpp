// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#include "../lv2/stereo_loop.hpp"
#include "rt_guard.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace loopi;
using namespace SooperLooper;
using namespace loopi::test;
struct Frame { float l = 0, r = 0; };
void require(bool value, const char* message)
{ if (!value) { throw std::runtime_error(message); } }
void near(float a, float b, float tolerance, const char* message)
{
    if (!std::isfinite(a) || std::abs(a - b) > tolerance) {
        std::cerr << message << ": " << a << " expected " << b << '\n';
        throw std::runtime_error(message);
    }
}
struct Rig {
    StereoLoop loop;
    LoopSettings settings;
    bool random = true;
    uint32_t rng = 1234567;
    std::array<float, 1024> in_l{}, in_r{}, out_l{}, out_r{};
    explicit Rig(double rate = 48000, float seconds = 40) : loop(rate, 128, seconds)
    { settings.safety_feedback = false; settings.fade_ms = 0; loop.configure(settings); }
    void tap(Command command) { loop.event(command, true); loop.event(command, false); }
    template <typename Source> std::vector<Frame> render(uint32_t frames, Source source)
    {
        std::vector<Frame> output(frames);
        for (uint32_t offset = 0; offset < frames;) {
            rng = 1664525 * rng + 1013904223;
            const uint32_t count = std::min(frames - offset, random ? 1 + rng % 1024 : 128);
            for (uint32_t i = 0; i < count; ++i) {
                auto value = source(offset + i);
                in_l[i] = value.l; in_r[i] = value.r;
            }
            {
                AudioGuard guard;
                loop.process(in_l.data(), in_r.data(), out_l.data(), out_r.data(), count);
            }
            require(loop.stereo_locked(), "stereo state/length/position lock");
            for (uint32_t i = 0; i < count; ++i) { output[offset + i] = {out_l[i], out_r[i]}; }
            offset += count;
        }
        require(heap_calls == 0, "allocation or free in audio processing");
        return output;
    }
    std::vector<Frame> render(uint32_t frames, Frame value = {})
    { return render(frames, [=](uint32_t) { return value; }); }
    void record(uint32_t frames = 4800, Frame value = {0.1f, -0.2f})
    {
        tap(Event::RECORD); render(frames, value);
        tap(Event::RECORD); render(frames);
        require(loop.output(State) == LooperStatePlaying, "record closes to Play");
    }
};

void record_play()
{
    Rig rig;
    rig.settings.fade_ms = 1.3f; rig.loop.configure(rig.settings);
    auto noise = [](uint32_t i) {
        uint32_t x = i * 747796405U + 2891336453U;
        x = ((x >> ((x >> 28) + 4)) ^ x) * 277803737U;
        const float l = float(x % 10000) * 0.00002f - 0.1f;
        return Frame{l, -2 * l};
    };
    rig.tap(Event::RECORD); rig.render(48000, noise);
    rig.tap(Event::RECORD); auto take = rig.render(48000);
    near(rig.loop.output(LoopLength), 1, 0.00001f, "1 second loop");
    for (uint32_t i = 128; i < take.size() - 128; ++i) {
        near(take[i].l, noise(i).l, 0.00001f, "noise playback L");
        near(take[i].r, noise(i).r, 0.00001f, "noise playback R");
    }
    rig.render(777);
    rig.tap(Event::TRIGGER); rig.render(128);
    near(rig.loop.output(LoopPosition), 128.0f / 48000, 0.00003f, "Trigger restarts");
    std::cout << "PASS record_play: one second stereo noise, fades, Trigger\n";
}

void overdub_undo()
{
    Rig rig;
    rig.record();
    rig.tap(Event::OVERDUB); rig.render(4800, {0.05f, -0.1f});
    rig.tap(Event::OVERDUB); auto take = rig.render(4800);
    near(take[2000].l, 0.15f, 0.00001f, "overdub adds layer");
    rig.tap(Event::UNDO); take = rig.render(4800);
    near(take[2000].l, 0.1f, 0.00001f, "Undo restores layer");
    rig.tap(Event::REDO); take = rig.render(4800);
    near(take[2000].l, 0.15f, 0.00001f, "Redo restores overdub");
    rig.settings.feedback = 0.5f; rig.loop.configure(rig.settings); rig.render(1000);
    rig.tap(Event::OVERDUB); rig.render(9600);
    rig.tap(Event::OVERDUB); take = rig.render(4800);
    near(take[2000].l, 0.0375f, 0.00002f, "feedback halves old layer each pass");
    rig.tap(Event::UNDO_ALL); take = rig.render(4800);
    near(take[2000].l, 0, 0, "Clear silence");
    near(rig.loop.output(LoopLength), 0, 0, "Clear length");
    rig.tap(Event::REDO_ALL); take = rig.render(4800);
    near(take[2000].l, 0.0375f, 0.00002f, "Redo all after Clear");
    std::cout << "PASS overdub/undo_redo: layer sum, feedback, history, Clear\n";
}

void gestures()
{
    Rig rig;
    rig.record();
    rig.loop.event(Event::OVERDUB, true); rig.render(48000, {0.01f, -0.02f});
    rig.loop.event(Event::OVERDUB, false); rig.render(128);
    require(rig.loop.output(State) == LooperStateOverdubbing, "exactly 1s is not long");
    rig.tap(Event::OVERDUB); rig.render(128);
    rig.loop.event(Event::OVERDUB, true); rig.render(48001);
    rig.loop.event(Event::OVERDUB, false); rig.render(128);
    require(rig.loop.output(State) == LooperStatePlaying, "held Overdub release ends command");
    rig.loop.event(Event::OVERDUB, true); rig.render(100);
    rig.tap(Event::TRIGGER); rig.render(48001);
    rig.loop.event(Event::OVERDUB, false); rig.render(128);
    require(rig.loop.output(State) == LooperStatePlaying, "long release does not restart command already Playing");
    rig.loop.event(Event::UNDO, true); rig.render(48001);
    rig.loop.event(Event::UNDO, false); rig.render(128);
    require(rig.loop.output(LoopLength) == 0, "long Undo clears");
    rig.loop.event(Event::REDO, true); rig.render(48001);
    rig.loop.event(Event::REDO, false); rig.render(128);
    require(rig.loop.output(LoopLength) > 0, "long Redo restores all");
    // Frame zero is a valid first press, and release retains double-tap timing.
    Rig one;
    one.loop.event(Event::RECORD_OR_OVERDUB, true); one.render(48001);
    one.loop.event(Event::RECORD_OR_OVERDUB, false); one.render(128);
    require(one.loop.output(LoopLength) == 0, "long Rec/Dub at frame zero clears");
    Rig two;
    two.record();
    two.tap(Event::OVERDUB); two.render(4800, {0.05f, -0.1f});
    two.tap(Event::OVERDUB); two.render(4800);
    two.tap(Event::OVERDUB); two.render(4800, {0.05f, -0.1f});
    two.tap(Event::OVERDUB); two.render(4800);
    two.tap(Event::RECORD_OR_OVERDUB); two.render(100, {0.025f, -0.05f});
    two.tap(Event::RECORD_OR_OVERDUB); auto take = two.render(4800);
    near(take[2000].l, 0.15f, 0.00002f, "double tap undoes two layers");
    std::cout << "PASS press_release: short, >1s, Undo/Redo all, frame zero, Rec/Dub double tap\n";
}

void threshold()
{
    Rig rig;
    rig.settings.threshold = 0.4f; rig.loop.configure(rig.settings);
    rig.tap(Event::RECORD);
    rig.render(100, {-0.8f, 0.8f});
    require(rig.loop.output(State) == LooperStateWaitStart, "threshold follows upstream positive leader input");
    rig.render(100, [](uint32_t i) { return Frame{i >= 73 ? 0.5f : 0.0f, -0.5f}; });
    near(rig.loop.output(LoopLength) * 48000, 27, 0.001f, "threshold starts at first crossing");
    rig.render(1000, {0.5f, -0.5f});
    rig.settings.threshold = 0; rig.loop.configure(rig.settings);
    rig.tap(Event::OVERDUB); rig.render(100);
    require(rig.loop.output(State) == LooperStateOverdubbing, "Overdub ends initial record");
    std::cout << "PASS threshold: sample-exact start, stereo sync, special ending\n";
}

std::vector<Frame> script(bool random)
{
    Rig rig;
    rig.random = random;
    rig.settings.fade_ms = 5; rig.loop.configure(rig.settings);
    std::vector<Frame> result;
    auto add = [&](uint32_t count, Frame value = {}) {
        auto take = rig.render(count, value); result.insert(result.end(), take.begin(), take.end());
    };
    rig.tap(Event::RECORD); add(8001, {0.1f, -0.2f});
    rig.tap(Event::RECORD); add(1234);
    rig.settings.input_gain = 0.25f; rig.settings.level = 0.7f;
    rig.settings.feedback = 0.5f; rig.loop.configure(rig.settings);
    rig.tap(Event::OVERDUB); add(16002, {0.03f, -0.04f});
    // Feedback moved by an LFO: values change mid-ramp and off step boundaries.
    for (uint32_t k = 0; k < 60; ++k) {
        rig.settings.feedback = 0.6f + 0.3f * std::sin(0.4f * float(k)); rig.loop.configure(rig.settings);
        add(97, {0.02f, -0.01f});
    }
    rig.tap(Event::OVERDUB); add(400);
    rig.tap(Event::UNDO); add(800);
    rig.tap(Event::REDO); add(800);
    rig.loop.enabled(false); add(700);
    rig.loop.enabled(true); add(700);
    rig.tap(Event::TRIGGER); add(1234);
    rig.tap(Event::UNDO_ALL); add(800);
    return result;
}

void slicing()
{
    const auto whole = script(false), sliced = script(true);
    require(whole.size() == sliced.size(), "script lengths");
    for (uint32_t i = 0; i < whole.size(); ++i) {
        if (whole[i].l != sliced[i].l || whole[i].r != sliced[i].r) {
            std::cerr << "first slice mismatch at " << i << ": " << whole[i].l << " / " << sliced[i].l << '\n';
            throw std::runtime_error("slicing must be bit-exact");
        }
    }
    std::cout << "PASS slicing: bit-exact commands, ramps, history fades and bypass\n";
}

void bypass()
{
    Rig rig;
    rig.record();
    rig.render(311);
    const auto before = rig.loop.output(LoopPosition);
    rig.loop.enabled(false); auto take = rig.render(960);
    float previous = 0.1f;
    for (const auto& sample : take) {
        require(std::abs(sample.l - previous) < 0.001f, "bypass fade has no discontinuity");
        previous = sample.l;
    }
    near(take.back().l, 0, 0, "bypassed loop silent");
    near(rig.loop.output(LoopPosition), std::fmod(before + 960.0f / 48000, 0.1f), 0.00003f, "muted position advances");
    rig.loop.enabled(true); take = rig.render(960);
    near(take.back().l, 0.1f, 0.00001f, "bypass restores sound");
    rig.tap(Event::MUTE); rig.render(960);
    rig.loop.enabled(false); rig.render(960);
    rig.loop.enabled(true); take = rig.render(960);
    require(rig.loop.output(State) == LooperStateMuted && take.back().l == 0, "pre-muted loop stays muted");
    for (double rate : {44100.0, 96000.0}) {
        Rig other(rate); other.record(uint32_t(rate / 10));
        near(other.loop.output(LoopLength), 0.1f, 0.00003f, "sample-rate independent loop time");
    }
    std::cout << "PASS bypass: mute/restore, continuous position, pre-muted, 44.1/96 kHz\n";
}

void levels()
{
    Rig rig;
    rig.settings.input_gain = 0.5f; rig.loop.configure(rig.settings);
    rig.render(1000); // Settle the input gain before recording.
    rig.record();
    auto take = rig.render(4800);
    near(take[2000].l, 0.05f, 0.00001f, "input gain affects recorded send");
    rig.settings.level = 0.5f; rig.loop.configure(rig.settings);
    take = rig.render(4800);
    near(take[2000].l, 0.025f, 0.00001f, "loop level scales playback");
    rig.settings.level = 1; rig.settings.input_gain = 1; rig.settings.safety_feedback = true;
    rig.loop.configure(rig.settings); rig.render(4800);
    rig.tap(Event::OVERDUB); rig.render(4800);
    rig.tap(Event::OVERDUB); take = rig.render(4800);
    near(take[2000].l, 0.048f, 0.00001f, "Safety feedback applies core 0.96 factor");
    rig.settings.play_feedback = true; rig.settings.feedback = 0.5f;
    rig.loop.configure(rig.settings); rig.render(14400);
    take = rig.render(4800);
    require(take[2000].l < 0.012f && take[2000].l > 0, "Play feedback decays playback");
    std::cout << "PASS levels: input gain, loop level, 0.96 safety feedback and Play feedback\n";
}

void memory_full()
{
    Rig rig(8000, 0.02f);
    rig.tap(Event::RECORD); rig.render(1024, {0.1f, -0.2f});
    require(rig.loop.output(State) == LooperStatePlaying, "memory full closes recording");
    near(rig.loop.output(LoopLength) * 8000, 256, 0.001f, "power-of-two capacity fully used");
    near(rig.loop.output(LoopFreeMemory), 0, 0, "memory full reports zero");
    std::cout << "PASS memory_full: exact capacity, clean close, zero free seconds\n";
}

void timing()
{
    Rig rig;
    rig.random = false;
    rig.record();
    std::vector<double> times;
    times.reserve(225000);
    uint32_t rng = 17;
    const auto start = std::chrono::steady_clock::now();
    // Ten simulated minutes, ONE loop: M2 will extend this to eight loops.
    for (uint32_t block = 0; block < 225000; ++block) {
        rng = 1664525 * rng + 1013904223;
        if (block % 503 == 0) {
            constexpr Command commands[]{Event::OVERDUB, Event::MUTE, Event::UNDO, Event::REDO, Event::TRIGGER};
            rig.tap(commands[rng % 5]);
        }
        // Worst case: an LFO moves Feedback every block, so it is always ramping.
        rig.settings.feedback = 0.75f + 0.2f * std::sin(0.01f * float(block)); rig.loop.configure(rig.settings);
        rig.in_l.fill(0.001f); rig.in_r.fill(-0.002f);
        const auto before = std::chrono::steady_clock::now();
        { AudioGuard guard; rig.loop.process(rig.in_l.data(), rig.in_r.data(), rig.out_l.data(), rig.out_r.data(), 128); }
        const auto after = std::chrono::steady_clock::now();
        times.push_back(std::chrono::duration<double, std::micro>(after - before).count());
        require(rig.loop.stereo_locked(), "timing stereo lock");
        require(std::isfinite(rig.out_l[127]) && std::isfinite(rig.out_r[127]), "timing finite output");
    }
    std::sort(times.begin(), times.end());
    require(heap_calls == 0, "timing audio heap calls");
    std::cout << "PASS rt_timing: 600 simulated seconds, 128 frames/48kHz, one stereo loop, feedback LFO; p99.9 "
              << times[std::size_t(times.size() * 0.999)] << " us, max " << times.back()
              << " us, wall " << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
              << " s; no audio heap calls\n";
}

int main(int argc, char** argv)
{
    try {
        if (argc == 2 && !std::strcmp(argv[1], "--timing")) { timing(); return 0; }
        record_play(); overdub_undo(); gestures(); threshold(); slicing(); bypass(); levels(); memory_full();
        std::cout << "PASS stereo_lock and RT allocation guard across all commands\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL core: " << error.what() << '\n'; return 1;
    }
}
