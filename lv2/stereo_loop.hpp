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
    // Core sync mode, chosen by LoopSet: 0 free, 1 synced, 2 relative sync.
    float sync = 0, quantize = 0, tempo = 0, eighths = 16, rate = 1;
    bool play_sync = false, round = false, mute_quantized = false, overdub_quantized = false,
         replace_quantized = false;
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
    // The sync buffers are in host time: the input feeds channel 0's core,
    // the output receives channel 0's sync output (Looper::run_loops).
    void process(const float* left, const float* right, float* out_l, float* out_r, uint32_t frames,
                 const float* sync_in = nullptr, float* sync_out = nullptr) noexcept;
    float output(SooperLooper::OutputPort port) const noexcept;
    // Channel 0's exact loop geometry in core frames, for the clock outputs.
    double position() const noexcept;
    double length() const noexcept;
    double cycle() const noexcept;
    bool reversed() const noexcept;
    float rate() const noexcept { return float(rate_value_); }
    // Host-frame offsets, within the last process() call, at which the loop
    // played the first frame of a cycle (or of the loop): for the clocks.
    uint32_t starts(bool loop, const uint32_t*& offsets) const noexcept
    { offsets = loop ? loop_at_.data() : cycle_at_.data(); return loop ? loop_count_ : cycle_count_; }
    uint64_t frames() const noexcept { return frames_; }
    bool stereo_locked() const noexcept;
    void recover() noexcept;
    void hit(Command command) noexcept { enqueue(command); }
    void finish_state() noexcept;

private:
    struct Request { Command command = SooperLooper::Event::UNKNOWN; bool bypass = false; };
    void enqueue(Command command, bool bypass = false) noexcept;
    bool transient() const noexcept;
    uint32_t safe_chunk(uint32_t requested) const noexcept;
    void run_core(uint32_t frames, Command command, bool bypass, uint32_t at) noexcept;
    // Runs the cores over core-time scratch [0, frames): inputs in 0, 1 and 6,
    // outputs in 2, 3 and 4. Requests, history fades and feedback steps split
    // the run exactly as in Milestone 1.
    void run_block(uint32_t frames) noexcept;
    void direct(const float* left, const float* right, float* out_l, float* out_r, uint32_t frames,
                const float* sync_in, float* sync_out) noexcept;
    void resampled(const float* left, const float* right, float* out_l, float* out_r, uint32_t frames,
                   const float* sync_in, float* sync_out) noexcept;
    float history(uint32_t channel, uint64_t index, uint64_t block_start) const noexcept;
    void track(uint32_t ran) noexcept;
    void boundary(uint64_t frame, bool loop) noexcept;
    void place(uint64_t block_start, uint32_t core_frames, uint32_t host_offset, bool resampled) noexcept;
    void keep_tail(uint64_t block_start) noexcept;
    std::array<LADSPA_Handle, 2> cores_{};
    std::array<std::array<float, SooperLooper::LASTPORT>, 2> ports_{};
    std::array<std::vector<float>, 7> scratch_;
    std::array<uint64_t, SooperLooper::Event::LAST_COMMAND> down_{};
    std::array<bool, SooperLooper::Event::LAST_COMMAND> held_{};
    std::array<Request, 16> requests_{};
    uint32_t head_ = 0, size_ = 0, maximum_, core_maximum_, ramp_samples_, fade_samples_ = 64;
    double rate_;
    // frames_ counts core frames (loop time), host_frames_ counts host frames;
    // they differ only while Rate is not 1. Press timing uses host time.
    uint64_t frames_ = 0, host_frames_ = 0, last_rec_press_ = 0;
    bool rec_pressed_ = false, configured_ = false, enabled_ = true, restore_ = false, second_undo_ = false;
    LoopSettings settings_{};
    Ramp input_gain_, level_, feedback_, gate_;
    // Varispeed (Looper::run_loops with SRC_LINEAR): linear interpolation of
    // the input into loop time and of the loop output back into host time.
    double rate_value_ = 1, in_phase_ = 0, out_pos_ = 0;
    bool resampling_ = false;
    std::array<float, 2> last_in_{};
    float sync_pending_ = 0;
    // Loop-time frames written up to each host frame of the current block.
    std::vector<uint32_t> emits_;
    // The last core outputs, indexed by core frame: frames_ - 8 to frames_ - 1.
    std::array<std::array<float, 8>, 2> tail_{};
    // Boundary tracking for the clocks, per core run (track()).
    struct Boundary { uint64_t frame; bool loop; };
    std::array<Boundary, 32> pending_{};
    uint32_t pending_count_ = 0, cycle_count_ = 0, loop_count_ = 0;
    std::array<uint32_t, 16> cycle_at_{}, loop_at_{};
    double track_pos_ = 0;
    bool track_back_ = false, track_valid_ = false;
};
} // namespace loopi
