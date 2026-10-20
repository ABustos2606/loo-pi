// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#pragma once
#include <lv2/atom/atom.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/options/options.h>
#include <lv2/urid/urid.h>
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace loopi {
// Discipline's host_config pattern; instantiate only. Oversized calls are
// chunked, so a missing/mistaken maximum never overruns scratch memory.
inline uint32_t scratch_size(const LV2_URID_Map& map, const LV2_Options_Option* options)
{
    uint32_t maximum = 0, nominal = 0;
    if (options) {
        const auto max_key = map.map(map.handle, LV2_BUF_SIZE__maxBlockLength);
        const auto nominal_key = map.map(map.handle, LV2_BUF_SIZE__nominalBlockLength);
        const auto int_type = map.map(map.handle, LV2_ATOM__Int);
        for (auto* option = options; option->key; ++option) {
            if (option->context != LV2_OPTIONS_INSTANCE || option->subject ||
                option->type != int_type || option->size != sizeof(int32_t) || !option->value) { continue; }
            int32_t value;
            std::memcpy(&value, option->value, sizeof(value));
            if (value <= 0) { continue; }
            if (option->key == max_key) { maximum = uint32_t(value); }
            if (option->key == nominal_key) { nominal = uint32_t(value); }
        }
    }
    return std::clamp(std::max(maximum, nominal ? nominal : 1024U), 1U, 8192U);
}
} // namespace loopi
