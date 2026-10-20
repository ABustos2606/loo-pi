// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
// A plugin instance with a worker the test runs by hand, shared by the
// multi-loop and Milestone 3 tests.
#pragma once
#include "host.hpp"
#include "rt_guard.hpp"
#include "../lv2/commands.hpp"
#include <algorithm>
#include <array>
#include <cstring>
extern "C" const LV2_Descriptor* lv2_descriptor(uint32_t);
namespace loopi::test {
using E = SooperLooper::Event;
struct Worker {
    struct Message { uint32_t size; std::array<uint8_t, 128> data; };
    std::array<Message, 32> jobs{}, answers{};
    uint32_t nj = 0, na = 0;
    bool reject = false;
    static LV2_Worker_Status schedule(LV2_Worker_Schedule_Handle h, uint32_t n, const void* data) {
        auto& w = *static_cast<Worker*>(h);
        if (w.reject || w.nj == w.jobs.size() || n > 128) return LV2_WORKER_ERR_NO_SPACE;
        auto& m = w.jobs[w.nj++]; m.size = n; std::memcpy(m.data.data(), data, n);
        return LV2_WORKER_SUCCESS;
    }
    static LV2_Worker_Status respond(LV2_Worker_Respond_Handle h, uint32_t n, const void* data) {
        auto& w = *static_cast<Worker*>(h);
        check(w.na < w.answers.size() && n <= 128, "worker response capacity");
        auto& m = w.answers[w.na++]; m.size = n; std::memcpy(m.data.data(), data, n);
        return LV2_WORKER_SUCCESS;
    }
    LV2_Worker_Schedule scheduler{this, schedule};
    LV2_Feature feature{LV2_WORKER__schedule, &scheduler};
    const LV2_Feature* features[3]{&map_feature, &feature, nullptr};
    void work(Host& h) {
        const auto* wi = static_cast<const LV2_Worker_Interface*>(h.descriptor->extension_data(LV2_WORKER__interface));
        for (uint32_t i = 0; i < nj; ++i) check(wi->work(h.handle, respond, this, jobs[i].size, jobs[i].data.data()) == LV2_WORKER_SUCCESS, "worker work");
        nj = 0;
    }
    void deliver(Host& h) {
        const auto* wi = static_cast<const LV2_Worker_Interface*>(h.descriptor->extension_data(LV2_WORKER__interface));
        AudioGuard guard;
        for (uint32_t i = 0; i < na; ++i) check(wi->work_response(h.handle, answers[i].size, answers[i].data.data()) == LV2_WORKER_SUCCESS, "worker response");
        na = 0;
    }
    void pump(Host& h) { work(h); deliver(h); }
};
struct Fixture {
    Worker worker;
    Host h{lv2_descriptor(0), worker.features};
    Fixture() { h[Port::dry_level][0] = h[Port::fade][0] = h[Port::safety_fb][0] = 0; h[Port::loop_secs][0] = 1; }
    ~Fixture() { worker.pump(h); }
    void run(uint32_t n = 128) { AudioGuard guard; h.run(n); }
    void advance(uint32_t frames) { while (frames) { auto n = std::min(128U,frames); run(n); frames -= n; } }
    void count(uint32_t n) { h[Port::loops][0] = float(n); run(); }
    void ready(uint32_t n) { count(n); worker.pump(h); run(); }
    float state(uint32_t n) { return h.buffers[kLoopBase + n * kLoopStride + 8][0]; }
    float length(uint32_t n) { return h.buffers[kLoopBase + n * kLoopStride + 11][0]; }
    void event(uint32_t command, uint32_t target = 0, bool press = true) {
        alignas(8) std::array<uint8_t, 64> data{};
        auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(data.data());
        seq->atom = {uint32_t(sizeof(seq->body) + sizeof(LV2_Atom_Event) + 8), map_uri(nullptr, LV2_ATOM__Sequence)};
        auto* ev = reinterpret_cast<LV2_Atom_Event*>(data.data() + sizeof(*seq));
        ev->body = {4, map_uri(nullptr, LV2_ATOM__Int)};
        const int32_t value = int32_t(target * 256 + command * 2 + press);
        std::memcpy(ev + 1, &value, 4);
        h.descriptor->connect_port(h.handle,index(Port::commands),data.data());
        run(1);
        h.descriptor->connect_port(h.handle,index(Port::commands),nullptr);
    }
    void record(uint32_t target, float level) {
        h[Port::in_l].fill(level); h[Port::in_r].fill(-level);
        event(E::RECORD,target); advance(1000); event(E::RECORD,target); advance(1000);
        h[Port::in_l].fill(0); h[Port::in_r].fill(0);
    }
};
} // namespace loopi::test
