// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
// Copyright (C) 2004 Jesse Chappell <jesse@essej.net>
#pragma once
#include "../src/plugin.hpp"
#include "../src/event.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace loopi {
using Command = SooperLooper::Event::command_t;

// Fixed sample duration rather than host-slice duration: Ingen may split a
// period anywhere. Looper::run_loops supplies the linear gain-ramp rule.
struct Ramp {
    float current = 1, target = 1, step = 0;
    uint32_t remaining = 0;
    void set(float value, uint32_t samples) noexcept;
    float next() noexcept;
};

struct LoopSettings {
    float level = 1, input_gain = 1, threshold = 0, feedback = 1, fade_ms = 1.3f;
    bool safety_feedback = true, play_feedback = false;
};

class StereoLoop {
public:
    StereoLoop(double rate, uint32_t maximum, float seconds = 40);
    ~StereoLoop();
    StereoLoop(const StereoLoop&) = delete;
    StereoLoop& operator=(const StereoLoop&) = delete;
    void configure(const LoopSettings& settings) noexcept;
    void event(Command command, bool press) noexcept;
    void enabled(bool value) noexcept;
    // Loop-only audio. Nullable inputs mean silence; nullable outputs discard.
    void process(const float* left, const float* right, float* out_l, float* out_r, uint32_t frames) noexcept;
    float output(SooperLooper::OutputPort port) const noexcept;
    uint64_t frames() const noexcept { return frames_; }
    bool stereo_locked() const noexcept;
    void recover() noexcept;

private:
    struct Request { Command command = SooperLooper::Event::UNKNOWN; bool bypass = false; };
    void enqueue(Command command, bool bypass = false) noexcept;
    bool transient() const noexcept;
    uint32_t safe_chunk(uint32_t requested) const noexcept;
    void run_core(uint32_t frames, Command command, bool bypass) noexcept;
    std::array<LADSPA_Handle, 2> cores_{};
    std::array<std::array<float, SooperLooper::LASTPORT>, 2> ports_{};
    std::array<std::vector<float>, 7> scratch_;
    std::array<uint64_t, SooperLooper::Event::LAST_COMMAND> down_{};
    std::array<bool, SooperLooper::Event::LAST_COMMAND> held_{};
    std::array<Request, 16> requests_{};
    uint32_t head_ = 0, size_ = 0, maximum_, ramp_samples_, fade_samples_ = 64;
    double rate_;
    uint64_t frames_ = 0, last_rec_press_ = 0;
    bool rec_pressed_ = false, configured_ = false, enabled_ = true, restore_ = false, second_undo_ = false;
    LoopSettings settings_{};
    Ramp input_gain_, level_, feedback_, gate_;
};
} // namespace loopi
