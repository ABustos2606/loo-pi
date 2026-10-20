// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#include "host.hpp"
#include "rt_guard.hpp"
#include "../lv2/commands.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>
extern "C" const LV2_Descriptor* lv2_descriptor(uint32_t);
using namespace loopi;
using namespace loopi::test;
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
void add_remove() {
    Fixture f;
    f.count(8); check(f.worker.nj == 7,"one job per added loop");
    for (uint32_t n=1;n<8;++n) check(f.state(n)==-2,"unallocated until worker answers");
    f.event(E::RECORD,9); f.worker.work(f.h); f.run();
    check(f.state(7)==-2,"work without response is still pending");
    f.worker.deliver(f.h); f.run();
    for (uint32_t n=0;n<8;++n) check(f.state(n)==0,"eight loops ready, absent command ignored");
    f.event(E::RECORD,1); f.advance(1000); f.event(E::RECORD,1); f.advance(1000);
    f.count(1); check(f.worker.nj==0,"remove waits for fade");
    f.advance(480); check(f.worker.nj==7,"retired memory sent to worker");
    f.count(8); // Re-add while deletion is queued: no use of stale pointers.
    f.worker.pump(f.h); f.run(); check(f.worker.nj==7,"re-add after deletion response");
    f.worker.pump(f.h); f.run();
    for(uint32_t n=1;n<8;++n) check(f.state(n)==0,"re-added empty");
    f.count(1); f.advance(480); f.worker.pump(f.h); f.run();
    f.worker.reject=true; f.count(4); check(f.worker.nj==0,"schedule rejection retains ownership");
    f.worker.reject=false; f.run(); check(f.worker.nj==3,"failed schedule retried");
    f.count(1); f.worker.pump(f.h); f.advance(1024); f.worker.pump(f.h); f.run();
    check(f.state(0)==4 && f.state(1)==-2,"late responses disposed, existing recording survives");
    std::cout<<"PASS add_remove: 1-8-1, delayed work/response, re-add during free, late allocation, rejected schedule\n";
}
void allocation_failure() {
    for (int point=0; point<20; ++point) {
        Fixture f; f.count(2);
        fail_after=point; f.worker.work(f.h); fail_after=-1;
        f.worker.deliver(f.h); f.run();
        check(f.state(1)==-2 || f.state(1)==0,"allocation failure telemetry");
        if(f.state(1)==-2) {
            f.advance(1024); check(f.worker.nj==0,"allocation failure does not spin");
            f.count(1);f.count(2);f.worker.pump(f.h);f.run();
            check(f.state(1)==0,"allocation retry after count change");
        }
    }
    std::cout<<"PASS allocation failure: object, scratch and partial core allocations, retry, cleanup\n";
}
void targets() {
    Fixture f; f.ready(4);
    f.event(E::RECORD,3); f.advance(100); check(f.state(1)==2 && f.state(0)==0,"explicit loop target");
    f.event(E::RECORD,3); f.advance(100);
    f.h[Port::selected][0]=3; f.run(); f.event(E::RECORD); check(f.state(2)==2,"selected input routes");
    f.event(kNextLoop); check(f.h[Port::selected_out][0]==4,"next selection");
    f.event(kNextLoop); check(f.h[Port::selected_out][0]==1,"next wraps");
    f.event(kPreviousLoop); check(f.h[Port::selected_out][0]==4,"previous wraps");
    f.run(); check(f.h[Port::selected_out][0]==4,"saved selected input does not reset navigation");
    f.h[Port::next_loop][0]=1; f.run(); check(f.h[Port::selected_out][0]==1,"next control edge");
    f.h[Port::next_loop][0]=0; f.run(); check(f.h[Port::selected_out][0]==1,"next release does not navigate");
    f.event(E::UNDO_ALL,1); f.advance(1000);
    for(uint32_t n=0;n<4;++n) check(f.length(n)==0,"all targets Clear");
    f.event(E::RECORD,1); f.advance(500); f.event(E::RECORD,1); f.advance(1000);
    for(uint32_t n=0;n<4;++n) check(f.state(n)==4 && f.length(n)>0,"all targets Record");
    f.count(2); check(f.h[Port::selected_out][0]<=2,"selection clamps after removal");
    std::cout<<"PASS targets: explicit, selected, all, Next/Previous wrapping and control edges\n";
}
void solo() {
    Fixture f; f.ready(3);
    f.record(2,.125f); f.record(3,.25f); f.record(4,.0625f); f.advance(1000);
    check(std::abs(f.h[Port::out_l][63]-.4375f)<1e-5,"sum all stereo loops");
    f.event(E::MUTE,4); f.advance(1000);
    f.event(E::SOLO,2); f.advance(1000);
    check(f.h[Port::soloed][0]==1 && f.state(1)==10,"solo mutes others");
    check(std::abs(f.h[Port::out_l][63]-.125f)<1e-5,"solo mix");
    f.event(E::SOLO,2,false); f.event(E::SOLO,2); f.advance(1000);
    check(f.h[Port::soloed][0]==0 && f.state(1)==4 && f.state(2)==10,"solo restores pre-muted states");
    f.event(E::SOLO,2,false); f.event(E::SOLO,2); f.advance(48001); f.event(E::SOLO,2,false); f.advance(1000);
    check(f.h[Port::soloed][0]==0,"long solo release restores");
    f.event(E::SOLO_NEXT); f.advance(1000);
    check(f.h[Port::selected_out][0]==2 && f.h[Port::soloed][0]==2,"solo next selects");
    f.count(1); f.advance(1000); check(f.h[Port::soloed][0]==0,"remove solo target restores other loops");
    std::cout<<"PASS solo: stereo mix, mute/restore, pre-muted, long release, solo-next, removed target\n";
}
void fading() {
    Fixture f; f.ready(2); f.record(3,.25f); f.advance(1000);
    f.count(1);
    float previous=f.h[Port::out_l][127];
    for(int n=0;n<500;++n) { f.run(1); const float value=f.h[Port::out_l][0]; check(std::abs(value-previous)<.001,"removal fade continuity"); previous=value; }
    check(previous==0,"removal ends silent"); f.worker.pump(f.h);
    std::cout<<"PASS removal fade: bounded sample changes, silent before worker free\n";
}
void slicing() {
    const auto render=[](bool random) {
        Fixture f; f.ready(3);
        f.h[Port::in_l].fill(.03125f);f.h[Port::in_r].fill(-.0625f);
        struct Action { uint32_t frame,cmd,target; };
        const Action actions[]{{0,E::RECORD,1},{1000,E::RECORD,1},{2000,E::SOLO,3},{3000,E::SOLO,3},
            {4000,E::OVERDUB,2},{7000,E::OVERDUB,2},{8000,E::UNDO,2},{9000,E::REDO,2},{10000,E::MUTE,4}};
        uint32_t frame=0,rng=17;std::vector<float> result;result.reserve(24000);
        const auto advance=[&](uint32_t end) {
            while(frame<end) {
                rng=rng*1664525+1013904223;
                const auto n=std::min(end-frame,random?1+rng%127:128U);
                f.run(n);
                for(uint32_t i=0;i<n;++i) { result.push_back(f.h[Port::out_l][i]);result.push_back(f.h[Port::out_r][i]); }
                frame+=n;
            }
        };
        for(const auto& a:actions) { advance(a.frame);f.event(a.cmd,a.target);++frame;
            result.push_back(f.h[Port::out_l][0]);result.push_back(f.h[Port::out_r][0]); }
        advance(12000);return result;
    };
    check(render(false)==render(true),"multi-loop random slicing changes audio");
    std::cout<<"PASS multi-loop slicing: exact stereo output with targets, Solo, history and mute\n";
}
void timing() {
    Fixture f; f.ready(8); f.record(1,.03125f);
    std::vector<double> times; times.reserve(225000);
    for(uint32_t n=0;n<225000;++n) {
        if(n%10000==0) f.event(E::SOLO_NEXT);
        const auto start=std::chrono::steady_clock::now(); f.run(128);
        times.push_back(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count());
    }
    std::sort(times.begin(),times.end());
    std::cout<<"PASS timing: 600 simulated seconds, 8 stereo loops; p99.9 "<<times[times.size()*999/1000]<<" us, max "<<times.back()<<" us\n";
}
int main(int argc,char**) {
    try {
        add_remove(); allocation_failure(); targets(); solo(); fading(); slicing();
        check(heap_calls==0,"no allocation/free during run or work_response");
        if(argc>1) timing();
        check(heap_calls==0,"no allocation/free during timing");
        std::cout<<"PASS multi-loop RT allocation guard\n";
    }catch(const std::exception& e){std::cerr<<"FAIL multi-loop: "<<e.what()<<'\n';return 1;}
}
