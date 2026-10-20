// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#include "commands.hpp"
#include "host_config.hpp"
#include "loop_state.hpp"
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
    LoopSet loop;
    StateKeys keys;
    double rate;
    CommandControls commands;
    std::array<std::vector<float>, 4> scratch;
    uint32_t maximum, ramp_samples;
    uint32_t sequence_type, int_type, frame_type;
    Ramp dry;
    bool configured = false;
    uint32_t selected_control = 0;

    Instance(double rate, uint32_t size, const LV2_URID_Map& map, const LV2_Worker_Schedule& schedule)
        : loop(rate, size, schedule), keys(map), rate(rate), maximum(size), ramp_samples(uint32_t(rate * 0.01)),
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
    const LV2_Worker_Schedule* schedule = nullptr;
    for (auto f = features; f && *f; ++f) {
        if (!(*f)->URI) { continue; }
        if (!std::strcmp((*f)->URI, LV2_URID__map)) {
            map = static_cast<const LV2_URID_Map*>((*f)->data);
        } else if (!std::strcmp((*f)->URI, LV2_WORKER__schedule)) {
            const auto* data = static_cast<const LV2_Worker_Schedule*>((*f)->data);
            schedule = data && data->schedule_work ? data : nullptr;
        } else if (!std::strcmp((*f)->URI, LV2_OPTIONS__options)) {
            options = static_cast<const LV2_Options_Option*>((*f)->data);
        }
    }
    if (!map || !map->map || !schedule || !std::isfinite(rate) || rate < 8000 || rate > 384000) { return nullptr; }
    try { return new Instance(rate, scratch_size(*map, options), *map, *schedule); }
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
        self.loop.count(uint32_t(self.control(Port::loops, 1, 1, 8)), self.control(Port::loop_secs, 40, 10, 300));
        const auto selection = uint32_t(self.control(Port::selected, 1, 1, 8));
        // Commands may select a loop without changing the saved input value.
        if (selection != self.selected_control) { self.loop.select(selection); self.selected_control = selection; }
        GlobalSettings global;
        global.quantize = std::round(self.control(Port::quantize, 0, 0, 3));
        global.round = self.control(Port::round, 0) >= .5f;
        global.mute_quantized = self.control(Port::mute_quant, 0) >= .5f;
        global.overdub_quantized = self.control(Port::dub_quant, 0) >= .5f;
        global.replace_quantized = self.control(Port::repl_quant, 0) >= .5f;
        global.sync_to = uint32_t(std::lround(self.control(Port::sync_to, 0, 0, 9)));
        global.relative_sync = self.control(Port::rel_sync, 0) >= .5f;
        global.eighths = std::round(self.control(Port::eighths, 16, 1, 128));
        global.bpm = self.control(Port::bpm, 120, 30, 300);
        self.loop.global(global);
        for (uint32_t n = 0; n < kMaxLoops; ++n) {
            const auto base = kLoopBase + n * kLoopStride;
            const auto ctl = [&](uint32_t p, float fallback) { return self.control(static_cast<Port>(base + p), fallback); };
            LoopSettings settings;
            settings.level = ctl(0, 1); settings.input_gain = ctl(1, 1);
            settings.threshold = ctl(2, 0); settings.feedback = ctl(3, 1);
            settings.fade_ms = self.control(Port::fade, 1.3f, 0, 50);
            settings.safety_feedback = self.control(Port::safety_fb, 1) >= .5f;
            settings.play_feedback = ctl(6, 0) >= .5f;
            settings.sync = ctl(4, 0) >= .5f ? 1 : 0;
            settings.play_sync = ctl(5, 0) >= .5f;
            const auto* rate = static_cast<const float*>(self.ports[base + 7]);
            settings.rate = rate && std::isfinite(*rate) ? std::clamp(*rate, .25f, 4.0f) : 1;
            self.loop.configure(n, settings, self.control(Port::enabled, 1) >= .5f);
        }
        self.dry.set(self.control(Port::dry_level, 1), self.configured ? self.ramp_samples : 0);
        self.configured = true;
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
        nonfinite |= self.loop.process(self.scratch[0].data(), self.scratch[1].data(), self.scratch[2].data(), self.scratch[3].data(), count,
                                       cycle ? cycle + offset : nullptr, loop ? loop + offset : nullptr);
        for (uint32_t i = 0; i < count; ++i) {
            const auto dry = self.dry.next();
            const float l = self.scratch[0][i] * dry + self.scratch[2][i];
            const float r = self.scratch[1][i] * dry + self.scratch[3][i];
            nonfinite |= !std::isfinite(l) || !std::isfinite(r);
            if (out_l) { out_l[offset + i] = l; }
            if (out_r) { out_r[offset + i] = r; }
        }
        offset += count;
      }
    };
    CommandEvents events(self.ports[index(Port::commands)], self.sequence_type, self.int_type, self.frame_type, frames);
    uint32_t frame;
    uint32_t target, command;
    bool press;
    while (events.next(frame, target, command, press)) {
        if (frame < offset) { continue; } // Ignore out-of-order timestamps.
        render(frame);
        self.loop.event(target, command, press);
    }
    render(frames);
    if (nonfinite) {
        if (out_l) { std::fill_n(out_l, frames, 0); }
        if (out_r) { std::fill_n(out_r, frames, 0); }
        // Individual nonfinite loop returns are recovered in LoopSet.
        // A finite sum can still overflow; silence this host slice.
    }
    self.output(index(Port::selected_out), float(self.loop.selected()));
    self.output(index(Port::soloed), float(self.loop.soloed()));
    self.output(index(Port::free_mem), self.loop.output(self.loop.selected() - 1, SooperLooper::LoopFreeMemory));
    constexpr std::array<SooperLooper::OutputPort, 6> telemetry{SooperLooper::State, SooperLooper::NextState,
        SooperLooper::Waiting, SooperLooper::LoopLength, SooperLooper::CycleLength, SooperLooper::LoopPosition};
    for (uint32_t n = 0; n < kMaxLoops; ++n) {
        const uint32_t base = kLoopBase + n * kLoopStride;
        for (uint32_t i = 0; i < telemetry.size(); ++i) {
            self.output(base + 8 + i, self.loop.output(n, telemetry[i]));
        }
    }
}

// Engine restarts its tempo counter with the engine; Loo-pi with activate.
void activate(LV2_Handle handle) { static_cast<Instance*>(handle)->loop.activate(); }
void cleanup(LV2_Handle handle) { delete static_cast<Instance*>(handle); }
LV2_Worker_Status work(LV2_Handle, LV2_Worker_Respond_Function respond, LV2_Worker_Respond_Handle handle, uint32_t size, const void* data)
{ return LoopSet::work(respond, handle, size, data); }
LV2_Worker_Status response(LV2_Handle handle, uint32_t size, const void* data)
{ return static_cast<Instance*>(handle)->loop.response(size, data); }
const LV2_Worker_Interface worker{work, response, nullptr};
// save may run while run() does (LV2 state's own threading class); restore
// is in the instantiation class.
LV2_State_Status save(LV2_Handle handle, LV2_State_Store_Function store, LV2_State_Handle state, uint32_t,
                      const LV2_Feature* const* features)
{
    auto& self = *static_cast<Instance*>(handle);
    return save_state(self.loop, self.keys, self.rate, store, state, features);
}
LV2_State_Status restore(LV2_Handle handle, LV2_State_Retrieve_Function retrieve, LV2_State_Handle state, uint32_t,
                         const LV2_Feature* const* features)
{
    auto& self = *static_cast<Instance*>(handle);
    return restore_state(self.loop, self.keys, retrieve, state, features);
}
const LV2_State_Interface state{save, restore};
const void* extension_data(const char* uri)
{
    if (!std::strcmp(uri, LV2_WORKER__interface)) { return &worker; }
    return !std::strcmp(uri, LV2_STATE__interface) ? &state : nullptr;
}
const LV2_Descriptor descriptor{kURI, instantiate, connect_port, activate, run, nullptr, cleanup, extension_data};
}
}

extern "C" LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{ return index == 0 ? &loopi::descriptor : nullptr; }
