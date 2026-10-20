// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#include "host.hpp"
#include <dlfcn.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <thread>

using namespace loopi;
using namespace loopi::test;

void audio(const LV2_Descriptor* d)
{
    Host host(d);
    uint32_t rng = 0x12345678;
    for (float dry : {0.0f, 0.25f, 1.0f}) {
        host[Port::dry_level][0] = dry;
        for (auto p : {Port::in_l, Port::in_r, Port::out_l, Port::out_r, Port::cycle_clock, Port::loop_clock}) {
            d->connect_port(host.handle, index(p), host[p].data());
        }
        host.run(480); // Fixed 10 ms Dry level ramp settles before equality checks.
        for (auto frames : {0U, 1U, 37U, 128U, 1024U}) {
            for (auto p : {Port::out_l, Port::out_r, Port::cycle_clock, Port::loop_clock}) { host[p].fill(99); }
            for (uint32_t i = 0; i < frames; ++i) {
                rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                host[Port::in_l][i] = float(rng % 1000) / 1000;
                host[Port::in_r][i] = -host[Port::in_l][i];
            }
            uint32_t offset = 0;
            do {
                const uint32_t slice = frames ? std::min(1 + rng % 43, frames - offset) : 0;
                for (auto p : {Port::in_l, Port::in_r, Port::out_l, Port::out_r, Port::cycle_clock, Port::loop_clock}) {
                    d->connect_port(host.handle, index(p), host[p].data() + offset);
                }
                host.run(slice);
                offset += slice;
            } while (offset < frames);
            for (uint32_t i = 0; i < frames; ++i) {
                check(host[Port::out_l][i] == host[Port::in_l][i] * dry, "left dry audio");
                check(host[Port::out_r][i] == host[Port::in_r][i] * dry, "right dry audio");
                check(host[Port::cycle_clock][i] == 0 && host[Port::loop_clock][i] == 0, "idle clocks");
            }
            for (auto p : {Port::out_l, Port::out_r, Port::cycle_clock, Port::loop_clock}) {
                check(host[p][frames] == 99, "slice boundary guard");
            }
        }
    }
    for (auto p : {Port::in_l, Port::in_r, Port::out_l, Port::out_r, Port::cycle_clock, Port::loop_clock}) {
        d->connect_port(host.handle, index(p), host[p].data());
    }
    // Reserved multi-loop and M3 controls remain inert in Milestone 1.
    host[Port::pause][0] = host[Port::next_loop][0] = host[Port::loops][0] = host[Port::selected][0] = 8;
    host.run(128);
    check(host[Port::selected_out][0] == 1 && host[Port::soloed][0] == 0, "idle selection");
    check(host[Port::free_mem][0] >= 40 && host[Port::free_mem][0] < 80, "40 second memory rounded up");
    for (uint32_t n = 0; n < kMaxLoops; ++n) {
        const auto base = kLoopBase + n * kLoopStride;
        check(host.buffers[base + 8][0] == (n ? -2 : 0), "idle/unallocated state");
        for (uint32_t p = 10; p < kLoopStride; ++p) { check(host.buffers[base + p][0] == 0, "idle telemetry"); }
    }
    d->connect_port(host.handle, index(Port::out_l), host[Port::in_l].data());
    d->connect_port(host.handle, index(Port::out_r), host[Port::in_r].data());
    const auto left = host[Port::in_l], right = host[Port::in_r];
    host.run(128);
    check(host[Port::in_l] == left && host[Port::in_r] == right, "in-place audio");
    for (float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        host[Port::in_l][0] = bad;
        host.run(1);
        check(host[Port::in_l][0] == 0, "finite output");
    }
    for (uint32_t p = 0; p < index(Port::count); ++p) { d->connect_port(host.handle, p, nullptr); }
    d->connect_port(host.handle, 999, nullptr);
    host.run(128); host.run(0);
    std::cout << "PASS audio: dry levels, random slices, zero frames, guards, in-place, finite, idle telemetry\n";
}

int main(int argc, char** argv)
{
    try {
        check(argc == 2, "usage: test_scaffold plugin.so");
        void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        if (!library) { throw std::runtime_error(dlerror()); }
        auto get = reinterpret_cast<const LV2_Descriptor* (*)(uint32_t)>(dlsym(library, "lv2_descriptor"));
        check(get && get(0) && !get(1), "descriptor discovery");
        const auto* d = get(0);
        check(!std::strcmp(d->URI, kURI), "plugin URI");
        check(!dlsym(library, "ladspa_descriptor") && !dlsym(library, "sl_init"), "core symbols hidden");
        const LV2_Feature* const no_worker[]{&map_feature, nullptr};
        const LV2_Feature* const no_map[]{&worker_feature, nullptr};
        for (auto f : {static_cast<const LV2_Feature* const*>(nullptr), no_worker, no_map}) {
            check(!d->instantiate(d, 48000, ".", f), "required feature rejection");
        }
        for (double rate : {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
            check(!d->instantiate(d, rate, ".", features), "invalid sample rate");
        }
        const auto* worker = static_cast<const LV2_Worker_Interface*>(d->extension_data(LV2_WORKER__interface));
        check(worker && worker->work && worker->work_response && !d->extension_data("urn:unknown"), "worker discovery");
        setenv("SL_SAMPLE_TIME", "garbage", 1); // Core allocation is explicitly per instance.
        audio(d);
        std::exception_ptr errors[2];
        auto repeated = [&](int n) {
            try { for (int i = 0; i < 8; ++i) { Host host(d); host.run(1); } }
            catch (...) { errors[n] = std::current_exception(); }
        };
        std::thread a(repeated, 0), b(repeated, 1);
        a.join(); b.join();
        for (auto error : errors) { if (error) { std::rethrow_exception(error); } }
        dlclose(library);
        std::cout << "PASS lifecycle: features, worker, concurrent instances, repeated allocation/cleanup\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL scaffold: " << e.what() << '\n'; return 1;
    }
}
