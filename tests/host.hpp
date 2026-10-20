// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#pragma once
#include "../lv2/ports.hpp"
#include <lv2/core/lv2.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>
#include <array>
#include <cstring>
#include <stdexcept>

namespace loopi::test {
inline void check(bool condition, const char* message)
{ if (!condition) { throw std::runtime_error(message); } }
inline LV2_URID map_uri(LV2_URID_Map_Handle, const char* uri)
{
    uint32_t id = 2166136261U;
    while (*uri) { id = (id ^ uint8_t(*uri++)) * 16777619U; }
    return id ? id : 1;
}
inline LV2_Worker_Status schedule_work(LV2_Worker_Schedule_Handle, uint32_t, const void*)
{ throw std::runtime_error("scaffold unexpectedly scheduled work"); }
inline LV2_URID_Map map{nullptr, map_uri};
inline LV2_Worker_Schedule schedule{nullptr, schedule_work};
inline LV2_Feature map_feature{LV2_URID__map, &map};
inline LV2_Feature worker_feature{LV2_WORKER__schedule, &schedule};
inline const LV2_Feature* features[]{&map_feature, &worker_feature, nullptr};

// Same dlopen/descriptor and reconnect-per-slice pattern as Discipline's host.
struct Host {
    const LV2_Descriptor* descriptor;
    LV2_Handle handle;
    std::array<std::array<float, 1025>, index(Port::count)> buffers{};
    explicit Host(const LV2_Descriptor* d, const LV2_Feature* const* host_features = features)
        : descriptor(d), handle(d->instantiate(d, 48000, ".", host_features))
    {
        check(handle, "instantiate");
        for (uint32_t p = 0; p < buffers.size(); ++p) { d->connect_port(handle, p, buffers[p].data()); }
        buffers[index(Port::dry_level)][0] = 1;
        buffers[index(Port::enabled)][0] = 1;
        buffers[index(Port::fade)][0] = 1.3f;
        buffers[index(Port::safety_fb)][0] = 1;
        for (uint32_t n = 0; n < kMaxLoops; ++n) {
            const auto base = kLoopBase + n * kLoopStride;
            buffers[base][0] = buffers[base + 1][0] = buffers[base + 3][0] = buffers[base + 7][0] = 1;
        }
        if (d->activate) { d->activate(handle); }
    }
    Host(const Host&) = delete;
    ~Host()
    {
        if (descriptor->deactivate) { descriptor->deactivate(handle); }
        descriptor->cleanup(handle);
    }
    auto& operator[](Port p) { return buffers[index(p)]; }
    void run(uint32_t frames) { descriptor->run(handle, frames); }
};
} // namespace loopi::test
