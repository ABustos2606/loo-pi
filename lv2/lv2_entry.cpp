// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#include "commands.hpp"
#include "host_config.hpp"
#include <lv2/core/lv2.h>
#include <lv2/worker/worker.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace loopi {
namespace {
struct Instance {
    std::array<void*, index(Port::count)> ports{};
    StereoLoop loop;
    CommandControls commands;
    std::array<std::vector<float>, 4> scratch;
    uint32_t maximum, ramp_samples;
    uint32_t sequence_type, int_type, frame_type;
    Ramp dry;
    bool configured = false;

    Instance(double rate, uint32_t size, const LV2_URID_Map& map)
        : loop(rate, size), maximum(size), ramp_samples(uint32_t(rate * 0.01)),
          sequence_type(map.map(map.handle, LV2_ATOM__Sequence)),
          int_type(map.map(map.handle, LV2_ATOM__Int)), frame_type(map.map(map.handle, LV2_ATOM__frameTime))
    { for (auto& buffer : scratch) { buffer.resize(size); } }
    float control(Port port, float fallback, float lo = 0, float hi = 1) const noexcept
    {
        const auto* value = static_cast<const float*>(ports[index(port)]);
        return value && std::isfinite(*value) ? std::clamp(*value, lo, hi) : fallback;
    }
    void output(uint32_t port, float value) noexcept
    { if (ports[port]) { *static_cast<float*>(ports[port]) = value; } }
};

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*, const LV2_Feature* const* features)
{
    const LV2_URID_Map* map = nullptr;
    const LV2_Options_Option* options = nullptr;
    bool schedule = false;
    for (auto f = features; f && *f; ++f) {
        if (!(*f)->URI) { continue; }
        if (!std::strcmp((*f)->URI, LV2_URID__map)) {
            map = static_cast<const LV2_URID_Map*>((*f)->data);
        } else if (!std::strcmp((*f)->URI, LV2_WORKER__schedule)) {
            const auto* data = static_cast<const LV2_Worker_Schedule*>((*f)->data);
            schedule = data && data->schedule_work;
        } else if (!std::strcmp((*f)->URI, LV2_OPTIONS__options)) {
            options = static_cast<const LV2_Options_Option*>((*f)->data);
        }
    }
    if (!map || !map->map || !schedule || !std::isfinite(rate) || rate < 8000 || rate > 384000) { return nullptr; }
    try { return new Instance(rate, scratch_size(*map, options), *map); }
    catch (...) { return nullptr; }
}

void connect_port(LV2_Handle handle, uint32_t port, void* data)
{
    auto& self = *static_cast<Instance*>(handle);
    if (port < self.ports.size()) { self.ports[port] = data; }
}

void run(LV2_Handle handle, uint32_t frames)
{
    auto& self = *static_cast<Instance*>(handle);
    // Zero frames publish telemetry without consuming edges or advancing time.
    // Never call the old core with zero samples (its deltas assume n > 0).
    if (frames) {
        LoopSettings settings;
        settings.level = self.control(Port::l1_level, 1);
        settings.input_gain = self.control(Port::l1_in_gain, 1);
        settings.threshold = self.control(Port::l1_thresh, 0);
        settings.feedback = self.control(Port::l1_feedback, 1);
        settings.fade_ms = self.control(Port::fade, 1.3f, 0, 50);
        settings.safety_feedback = self.control(Port::safety_fb, 1) >= 0.5f;
        settings.play_feedback = self.control(Port::l1_play_fb, 0) >= 0.5f;
        self.loop.configure(settings);
        self.dry.set(self.control(Port::dry_level, 1), self.configured ? self.ramp_samples : 0);
        self.configured = true;
        self.loop.enabled(self.control(Port::enabled, 1) >= 0.5f);
        self.commands.update(self.ports, self.loop);
    }
    const auto* left = static_cast<const float*>(self.ports[index(Port::in_l)]);
    const auto* right = static_cast<const float*>(self.ports[index(Port::in_r)]);
    auto* out_l = static_cast<float*>(self.ports[index(Port::out_l)]);
    auto* out_r = static_cast<float*>(self.ports[index(Port::out_r)]);
    auto* cycle = static_cast<float*>(self.ports[index(Port::cycle_clock)]);
    auto* loop = static_cast<float*>(self.ports[index(Port::loop_clock)]);
    bool nonfinite = false;
    uint32_t offset = 0;
    const auto render = [&](uint32_t end) {
      while (offset < end) {
        const uint32_t count = std::min(end - offset, self.maximum);
        // Preserve the original dry input with in-place host buffers.
        for (uint32_t i = 0; i < count; ++i) {
            self.scratch[0][i] = left && std::isfinite(left[offset + i]) ? left[offset + i] : 0;
            self.scratch[1][i] = right && std::isfinite(right[offset + i]) ? right[offset + i] : 0;
        }
        self.loop.process(self.scratch[0].data(), self.scratch[1].data(), self.scratch[2].data(), self.scratch[3].data(), count);
        for (uint32_t i = 0; i < count; ++i) {
            const auto dry = self.dry.next();
            const float l = self.scratch[0][i] * dry + self.scratch[2][i];
            const float r = self.scratch[1][i] * dry + self.scratch[3][i];
            nonfinite |= !std::isfinite(l) || !std::isfinite(r);
            if (out_l) { out_l[offset + i] = l; }
            if (out_r) { out_r[offset + i] = r; }
            if (cycle) { cycle[offset + i] = 0; }
            if (loop) { loop[offset + i] = 0; }
        }
        offset += count;
      }
    };
    CommandEvents events(self.ports[index(Port::commands)], self.sequence_type, self.int_type, self.frame_type, frames);
    uint32_t frame;
    Command command;
    bool press;
    while (events.next(frame, command, press)) {
        if (frame < offset) { continue; } // Ignore out-of-order timestamps.
        render(frame);
        self.loop.event(command, press);
    }
    render(frames);
    if (nonfinite) {
        if (out_l) { std::fill_n(out_l, frames, 0); }
        if (out_r) { std::fill_n(out_r, frames, 0); }
        self.loop.recover();
    }
    self.output(index(Port::selected_out), 1);
    self.output(index(Port::soloed), 0);
    self.output(index(Port::free_mem), self.loop.output(SooperLooper::LoopFreeMemory));
    constexpr std::array<SooperLooper::OutputPort, 6> telemetry{SooperLooper::State, SooperLooper::NextState,
        SooperLooper::Waiting, SooperLooper::LoopLength, SooperLooper::CycleLength, SooperLooper::LoopPosition};
    for (uint32_t n = 0; n < kMaxLoops; ++n) {
        const uint32_t base = kLoopBase + n * kLoopStride;
        for (uint32_t i = 0; i < telemetry.size(); ++i) {
            self.output(base + 8 + i, n ? (i < 2 ? -2.0f : 0.0f) : self.loop.output(telemetry[i]));
        }
    }
}

void cleanup(LV2_Handle handle) { delete static_cast<Instance*>(handle); }
// Reserved until M2. No jobs are scheduled by the single-loop layer.
LV2_Worker_Status work(LV2_Handle, LV2_Worker_Respond_Function, LV2_Worker_Respond_Handle, uint32_t, const void*)
{ return LV2_WORKER_ERR_UNKNOWN; }
LV2_Worker_Status response(LV2_Handle, uint32_t, const void*) { return LV2_WORKER_ERR_UNKNOWN; }
const LV2_Worker_Interface worker{work, response, nullptr};
const void* extension_data(const char* uri)
{ return !std::strcmp(uri, LV2_WORKER__interface) ? &worker : nullptr; }
const LV2_Descriptor descriptor{kURI, instantiate, connect_port, nullptr, run, nullptr, cleanup, extension_data};
}
}

extern "C" LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{ return index == 0 ? &loopi::descriptor : nullptr; }
