// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
// Copyright (C) 2004 Jesse Chappell <jesse@essej.net>
#pragma once
#include "ports.hpp"
#include "stereo_loop.hpp"
#include <lv2/worker/worker.h>
#include <atomic>

namespace loopi {
// Next/Previous are Engine controls upstream, not Event::command_t entries.
// Reserve explicit wire IDs outside the upstream enum; never cast these to it.
inline constexpr uint32_t kNextLoop = 64, kPreviousLoop = 65;

// Settings Engine holds for every loop.
struct GlobalSettings {
    float quantize = 0, eighths = 16, bpm = 120;
    uint32_t sync_to = 0; // 0 None, 1 Tempo, 2 + N for loop N + 1
    bool round = false, mute_quantized = false, overdub_quantized = false, replace_quantized = false,
         relative_sync = false;
};

class LoopSet {
public:
    LoopSet(double rate, uint32_t maximum, const LV2_Worker_Schedule& schedule);
    ~LoopSet();
    void count(uint32_t value, float seconds) noexcept;
    void select(uint32_t value) noexcept;
    void event(uint32_t target, uint32_t command, bool press) noexcept;
    void global(const GlobalSettings& settings) noexcept;
    // settings.sync is the loop's own Sync switch (0 or 1); the core's sync
    // mode also depends on the sync source (see configure()).
    void configure(uint32_t n, const LoopSettings& settings, bool enabled) noexcept;
    void activate() noexcept;
    // Nullable clock outputs receive the cycle and loop triggers.
    bool process(const float* l, const float* r, float* out_l, float* out_r, uint32_t frames,
                 float* cycle_clock = nullptr, float* loop_clock = nullptr) noexcept;
    float output(uint32_t n, SooperLooper::OutputPort port) const noexcept;
    uint32_t selected() const noexcept { return selected_ + 1; }
    uint32_t soloed() const noexcept { return solo_ < 0 ? 0 : uint32_t(solo_ + 1); }
    static LV2_Worker_Status work(LV2_Worker_Respond_Function respond, LV2_Worker_Respond_Handle handle,
                                  uint32_t size, const void* data) noexcept;
    LV2_Worker_Status response(uint32_t size, const void* data) noexcept;

    // LV2 state save, on the host's thread while run() goes on. Between
    // begin_save() and end_save() no loop that shared() returned is freed.
    void begin_save() noexcept { saving_.store(true); }
    void end_save() noexcept { saving_.store(false); }
    const StereoLoop* shared(uint32_t n) const noexcept { return shared_[n].load(); }
    uint32_t shared_count() const noexcept { return shared_count_.load(std::memory_order_relaxed); }
    // LV2 state restore (instantiation class). Loop n gets the frames, or is
    // cleared when there are none. False if its memory can't be allocated.
    bool restore(uint32_t n, const float* left, const float* right, uint64_t frames, uint64_t cycle, float seconds) noexcept;
    // Keep `value` loops until the Loops control has reached it or changed:
    // Ingen restores state before it sets the saved control values.
    void hold(uint32_t value) noexcept;
private:
    struct Slot {
        StereoLoop* loop = nullptr;
        bool pending = false, failed = false, retiring = false, enabled = true;
        Ramp gain;
    };
    struct Job {
        uint32_t index, maximum;
        double rate;
        float seconds;
        StereoLoop* dispose;
        bool orphan; // a late allocation for a restored slot, only freed
    };
    struct Answer { uint32_t index; StereoLoop* loop; bool allocation, orphan; };
    void share(uint32_t n) noexcept;
    void service() noexcept;
    void solo(int target, bool value, bool retrigger = false) noexcept;
    void hit(uint32_t n, Command command) noexcept;
    int sync_loop() const noexcept;
    void generate_sync(uint32_t frames) noexcept;
    void clocks(float* cycle_clock, float* loop_clock, uint32_t frames) noexcept;
    std::array<Slot, kMaxLoops> slots_{};
    std::array<bool, kMaxLoops> pre_solo_{};
    std::array<std::vector<float>, 2> scratch_;
    std::vector<float> sync_;
    GlobalSettings global_{};
    // Engine::generate_sync state; Engine::set_tempo resets it.
    double tempo_counter_ = 0, quarter_counter_ = 0;
    float counted_bpm_ = 0;
    // Clock outputs: samples left of the current pulses.
    uint32_t cycle_left_ = 0, loop_left_ = 0, pulse_samples_ = 48;
    LV2_Worker_Schedule schedule_;
    double rate_;
    uint32_t maximum_, ramp_samples_, count_ = 1, selected_ = 0;
    int solo_ = -1;
    float seconds_ = 40;
    uint64_t frames_ = 0, solo_down_ = 0;
    bool solo_held_ = false;
    // Saving: the loops the save thread may read, and the guard against
    // freeing them (Dekker: unshare, then check saving_; seq_cst on both sides).
    std::array<std::atomic<StereoLoop*>, kMaxLoops> shared_{};
    std::atomic<uint32_t> shared_count_{1};
    std::atomic<bool> saving_{false};
    std::array<StereoLoop*, kMaxLoops> orphans_{};
    uint32_t hold_ = 0, held_first_ = 0;
    bool held_seen_ = false;
};
} // namespace loopi
