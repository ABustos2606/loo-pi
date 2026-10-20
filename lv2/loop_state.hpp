// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#pragma once
#include "loop_set.hpp"
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>
#include <array>
#include <cstdint>
#include <vector>

namespace loopi {
// The keys are permanent once a patch has been saved with them.
struct StateKeys {
    LV2_URID loops, path, int_type, long_type, float_type;
    std::array<LV2_URID, kMaxLoops> file{}, cycle{}, memory{};
    explicit StateKeys(const LV2_URID_Map& map);
};

// Looper::save_loop and Looper::load_loop, through LV2 state: each loop with
// audio goes to loopN.wav beside the state, 32-bit float stereo.
LV2_State_Status save_state(LoopSet& loops, const StateKeys& keys, double rate, LV2_State_Store_Function store,
                            LV2_State_Handle handle, const LV2_Feature* const* features) noexcept;
LV2_State_Status restore_state(LoopSet& loops, const StateKeys& keys, LV2_State_Retrieve_Function retrieve,
                               LV2_State_Handle handle, const LV2_Feature* const* features) noexcept;

// WAV files. Writes 32-bit float. Reads 16, 24 and 32-bit PCM and 32-bit
// float; a mono file is copied to both channels, as Looper::load_loop does.
bool write_wav(const char* path, const std::vector<float>& interleaved, uint32_t rate);
bool read_wav(const char* path, std::vector<float>& left, std::vector<float>& right, uint32_t& rate);
} // namespace loopi
