// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
// Copyright (C) 2004 Jesse Chappell <jesse@essej.net>
#include "loop_set.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace loopi {
using namespace SooperLooper;
LoopSet::LoopSet(double rate, uint32_t maximum, const LV2_Worker_Schedule& schedule)
    : schedule_(schedule), rate_(rate), maximum_(maximum), ramp_samples_(std::max(1U, uint32_t(rate * .01)))
{
    for (auto& s : scratch_) { s.resize(maximum); }
    slots_[0].loop = new StereoLoop(rate, maximum);
}
LoopSet::~LoopSet()
{ for (auto& slot : slots_) { delete slot.loop; } }

void LoopSet::count(uint32_t value, float seconds) noexcept
{
    seconds_ = seconds;
    if (value != count_) {
        if (solo_ >= int(value)) { solo(solo_, false); }
        count_ = value;
        selected_ = std::min(selected_, count_ - 1);
        for (auto& slot : slots_) { slot.failed = false; }
    }
    service();
}
void LoopSet::service() noexcept
{
    for (uint32_t n = 0; n < kMaxLoops; ++n) {
        auto& s = slots_[n];
        if (s.pending) { continue; }
        if (n < count_) {
            if (s.loop) {
                if (s.retiring) { s.retiring = false; s.gain.set(1, ramp_samples_); }
                continue;
            }
            if (s.failed) { continue; }
        } else {
            if (!s.loop) { continue; }
            if (!s.retiring) { s.retiring = true; s.gain.set(0, ramp_samples_); }
            if (s.gain.remaining) { continue; }
        }
        Job job{n, maximum_, rate_, seconds_, n < count_ ? nullptr : s.loop};
        // Set ownership before schedule_work: a host may execute work inline.
        auto* previous = s.loop;
        s.pending = true;
        if (job.dispose) { s.loop = nullptr; }
        if (schedule_.schedule_work(schedule_.handle, sizeof(job), &job) != LV2_WORKER_SUCCESS) {
            s.pending = false;
            s.loop = previous; // Schedule failure leaves ownership on the audio side.
        }
    }
}
LV2_Worker_Status LoopSet::work(LV2_Worker_Respond_Function respond, LV2_Worker_Respond_Handle handle,
                                uint32_t size, const void* data) noexcept
{
    if (size != sizeof(Job) || !data || !respond) { return LV2_WORKER_ERR_UNKNOWN; }
    Job job; std::memcpy(&job, data, sizeof(job));
    if (job.index >= kMaxLoops) { return LV2_WORKER_ERR_UNKNOWN; }
    Answer answer{job.index, nullptr, !job.dispose};
    if (job.dispose) { delete job.dispose; }
    else {
        try { answer.loop = new StereoLoop(job.rate, job.maximum, job.seconds); }
        catch (...) {} // Report -2 on allocation failure, never throw through LV2.
    }
    const auto status = respond(handle, sizeof(answer), &answer);
    if (status != LV2_WORKER_SUCCESS) { delete answer.loop; }
    return status;
}
LV2_Worker_Status LoopSet::response(uint32_t size, const void* data) noexcept
{
    if (size != sizeof(Answer) || !data) { return LV2_WORKER_ERR_UNKNOWN; }
    Answer answer; std::memcpy(&answer, data, sizeof(answer));
    if (answer.index >= kMaxLoops || !slots_[answer.index].pending) { return LV2_WORKER_ERR_UNKNOWN; }
    auto& s = slots_[answer.index];
    s.pending = false;
    s.failed = answer.allocation && !answer.loop;
    s.loop = answer.loop;
    s.retiring = false;
    s.gain = Ramp{};
    pre_solo_[answer.index] = false;
    s.enabled = true;
    // A late allocation after removal is retained here, then retired by service.
    // No allocation, deletion, port connection or page touch on this thread.
    if (s.loop && solo_ >= 0 && answer.index != uint32_t(solo_)) { hit(answer.index, Event::MUTE_ON); }
    return LV2_WORKER_SUCCESS;
}
void LoopSet::select(uint32_t value) noexcept { selected_ = std::min(value - 1, count_ - 1); }
void LoopSet::hit(uint32_t n, Command command) noexcept
{ if (slots_[n].loop) { slots_[n].loop->hit(command); } }

void LoopSet::solo(int target, bool value, bool retrigger) noexcept
{
    // Engine::do_global_rt_event / Looper::set_soloed. Keep a snapshot until
    // solo ends, including when moving between targets. Upstream's OR test for
    // Muted/OffMuted is always true; use the intended AND so pre-muted loops
    // remain muted when restored (as with the bypass adapter).
    for (uint32_t n = 0; n < count_; ++n) {
        if (!slots_[n].loop) { continue; }
        const auto state = output(n, State);
        if (value) {
            if (solo_ < 0) { pre_solo_[n] = state == LooperStateMuted || state == LooperStateOffMuted; }
            if (int(n) != target) { hit(n, Event::MUTE_ON); }
            else if (state == LooperStateMuted) { hit(n, retrigger ? Event::TRIGGER : Event::MUTE_OFF); }
        } else if (int(n) != target && !pre_solo_[n]) { hit(n, Event::MUTE_OFF); }
    }
    solo_ = value ? target : -1;
}
void LoopSet::event(uint32_t target, uint32_t command, bool press) noexcept
{
    if (target > count_ + 1) { return; }
    if (command == kNextLoop || command == kPreviousLoop) {
        if (press) { selected_ = (selected_ + (command == kNextLoop ? 1 : count_ - 1)) % count_; }
        return;
    }
    if (command >= Event::LAST_COMMAND) { return; }
    auto cmd = static_cast<Command>(command);
    int chosen = target == 0 ? int(selected_) : int(target) - 2;
    const bool excl = cmd >= Event::RECORD_EXCLUSIVE && cmd <= Event::RECORD_OR_OVERDUB_EXCL_PREV;
    const bool rec_solo = cmd == Event::RECORD_SOLO || cmd == Event::RECORD_SOLO_NEXT || cmd == Event::RECORD_SOLO_PREV;
    const bool dub_solo = cmd >= Event::RECORD_OR_OVERDUB_SOLO && cmd <= Event::RECORD_OR_OVERDUB_SOLO_PREV;
    if (cmd == Event::SOLO || cmd == Event::SOLO_NEXT || cmd == Event::SOLO_PREV || excl || rec_solo || dub_solo) {
        const bool act = press || (solo_held_ && frames_ - solo_down_ > uint64_t(rate_));
        const bool next = cmd == Event::SOLO_NEXT || cmd == Event::RECORD_SOLO_NEXT || cmd == Event::RECORD_EXCLUSIVE_NEXT || cmd == Event::RECORD_OR_OVERDUB_EXCL_NEXT || cmd == Event::RECORD_OR_OVERDUB_SOLO_NEXT;
        const bool prev = cmd == Event::SOLO_PREV || cmd == Event::RECORD_SOLO_PREV || cmd == Event::RECORD_EXCLUSIVE_PREV || cmd == Event::RECORD_OR_OVERDUB_EXCL_PREV || cmd == Event::RECORD_OR_OVERDUB_SOLO_PREV;
        if (act && (next || prev)) { selected_ = (selected_ + (next ? 1 : count_ - 1)) % count_; chosen = int(selected_); }
        if (chosen < 0 || !slots_[chosen].loop) { return; }
        const bool muted = output(chosen, State) == LooperStateMuted && output(chosen, LoopLength) > 0;
        if (act) {
            if (excl) {
                for (uint32_t n = 0; n < count_; ++n) { if (int(n) != chosen && slots_[n].loop) { slots_[n].loop->finish_state(); } }
            } else { solo(chosen, dub_solo || solo_ != chosen, cmd == Event::RECORD_OR_OVERDUB_SOLO_TRIG || cmd == Event::RECORD_OVERDUB_END_SOLO_TRIG); }
            if (excl || dub_solo) { selected_ = uint32_t(chosen); }
            solo_down_ = frames_;
        }
        solo_held_ = press;
        if (rec_solo || (excl && cmd <= Event::RECORD_EXCLUSIVE_PREV)) { cmd = Event::RECORD; }
        else if (excl || (dub_solo && (!muted || next || prev))) {
            cmd = cmd == Event::RECORD_OVERDUB_END_SOLO || cmd == Event::RECORD_OVERDUB_END_SOLO_TRIG
                ? Event::RECORD_OVERDUB_END_SOLO : Event::RECORD_OR_OVERDUB;
        }
        else { return; }
        slots_[chosen].loop->event(cmd, press);
        return;
    }
    for (uint32_t n = 0; n < count_; ++n) {
        if ((target == 1 || int(n) == chosen) && slots_[n].loop) { slots_[n].loop->event(cmd, press); }
    }
}
void LoopSet::configure(uint32_t n, const LoopSettings& settings, bool enabled) noexcept
{
    auto& s = slots_[n];
    if (!s.loop) { return; }
    s.loop->configure(settings); s.loop->enabled(enabled);
    if (enabled && !s.enabled && solo_ >= 0 && int(n) != solo_) { hit(n, Event::MUTE_ON); }
    s.enabled = enabled;
}
bool LoopSet::process(const float* l, const float* r, float* out_l, float* out_r, uint32_t frames) noexcept
{
    bool nonfinite = false;
    std::fill_n(out_l, frames, 0); std::fill_n(out_r, frames, 0);
    for (auto& s : slots_) {
        if (!s.loop) { continue; }
        s.loop->process(l, r, scratch_[0].data(), scratch_[1].data(), frames);
        bool bad = false;
        for (uint32_t i = 0; i < frames; ++i) { bad |= !std::isfinite(scratch_[0][i]) || !std::isfinite(scratch_[1][i]); }
        if (bad) { s.loop->recover(); }
        nonfinite |= bad;
        for (uint32_t i = 0; i < frames; ++i) {
            const auto gain = s.gain.next();
            if (!bad) { out_l[i] += scratch_[0][i] * gain; out_r[i] += scratch_[1][i] * gain; }
        }
    }
    frames_ += frames;
    service();
    return nonfinite;
}
float LoopSet::output(uint32_t n, OutputPort port) const noexcept
{
    if (n >= count_ || !slots_[n].loop) { return port == State || port == NextState ? -2.0f : 0; }
    return slots_[n].loop->output(port);
}
} // namespace loopi
