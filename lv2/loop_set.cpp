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
    pulse_samples_ = std::max(1U, uint32_t(std::lround(rate * .001))); // 1 ms clock trigger
    for (auto& s : scratch_) { s.resize(maximum); }
    sync_.resize(maximum);
    slots_[0].loop = new StereoLoop(rate, maximum);
    share(0);
}
LoopSet::~LoopSet()
{
    for (auto& slot : slots_) { delete slot.loop; }
    for (auto* loop : orphans_) { delete loop; }
}
void LoopSet::share(uint32_t n) noexcept
{
    const auto& s = slots_[n];
    shared_[n].store(n < count_ && !s.retiring ? s.loop : nullptr);
}

void LoopSet::count(uint32_t value, float seconds) noexcept
{
    seconds_ = seconds;
    if (hold_) {
        if (value == hold_ || (held_seen_ && value != held_first_)) { hold_ = 0; held_seen_ = false; }
        else {
            if (!held_seen_) { held_seen_ = true; held_first_ = value; }
            value = hold_;
        }
    }
    if (value != count_) {
        if (solo_ >= int(value)) { solo(solo_, false); }
        count_ = value;
        selected_ = std::min(selected_, count_ - 1);
        for (auto& slot : slots_) { slot.failed = false; }
        shared_count_.store(count_, std::memory_order_relaxed);
    }
    service();
}
void LoopSet::service() noexcept
{
    for (uint32_t n = 0; n < kMaxLoops; ++n) {
        if (!orphans_[n]) { continue; }
        Job job{n, maximum_, rate_, seconds_, orphans_[n], true};
        if (schedule_.schedule_work(schedule_.handle, sizeof(job), &job) == LV2_WORKER_SUCCESS) { orphans_[n] = nullptr; }
    }
    for (uint32_t n = 0; n < kMaxLoops; ++n) {
        auto& s = slots_[n];
        if (s.pending) { continue; }
        if (n < count_) {
            if (s.loop) {
                if (s.retiring) { s.retiring = false; s.gain.set(1, ramp_samples_); share(n); }
                continue;
            }
            if (s.failed) { continue; }
        } else {
            if (!s.loop) { continue; }
            if (!s.retiring) { s.retiring = true; s.gain.set(0, ramp_samples_); share(n); }
            if (s.gain.remaining) { continue; }
            // Unshared above; a save that read the pointer before that holds
            // saving_, so freeing waits until it ends.
            if (saving_.load()) { continue; }
        }
        Job job{n, maximum_, rate_, seconds_, n < count_ ? nullptr : s.loop, false};
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
    Answer answer{job.index, nullptr, !job.dispose, job.orphan};
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
    if (answer.index >= kMaxLoops) { return LV2_WORKER_ERR_UNKNOWN; }
    if (answer.orphan) { return LV2_WORKER_SUCCESS; }
    if (!slots_[answer.index].pending) { return LV2_WORKER_ERR_UNKNOWN; }
    auto& s = slots_[answer.index];
    s.pending = false;
    if (s.loop) {
        // Restored while the job was out: keep the restored loop and free the
        // late allocation through the worker (a removal's answer has none).
        if (answer.loop) {
            Job job{answer.index, maximum_, rate_, seconds_, answer.loop, true};
            if (schedule_.schedule_work(schedule_.handle, sizeof(job), &job) != LV2_WORKER_SUCCESS) {
                orphans_[answer.index] = answer.loop;
            }
        }
        return LV2_WORKER_SUCCESS;
    }
    s.failed = answer.allocation && !answer.loop;
    s.loop = answer.loop;
    s.retiring = false;
    s.gain = Ramp{};
    pre_solo_[answer.index] = false;
    s.enabled = true;
    share(answer.index);
    // A late allocation after removal is retained here, then retired by service.
    // No allocation, deletion, port connection or page touch on this thread.
    if (s.loop && solo_ >= 0 && answer.index != uint32_t(solo_)) { hit(answer.index, Event::MUTE_ON); }
    return LV2_WORKER_SUCCESS;
}
bool LoopSet::restore(uint32_t n, const float* left, const float* right, uint64_t frames, uint64_t cycle,
                      float seconds) noexcept
{
    auto& s = slots_[n];
    solo_ = -1;
    pre_solo_ = {};
    if (!frames) {
        if (s.loop) { s.loop->hit(Event::UNDO_ALL); }
        return true;
    }
    // Looper::load_loop needs the file to fit the free memory. A restored loop
    // gets its saved memory, or more if the file is longer.
    const float needed = float(double(frames) / rate_) + 1;
    StereoLoop* loop = nullptr;
    try { loop = new StereoLoop(rate_, maximum_, std::max(seconds, needed)); }
    catch (...) { return false; }
    loop->load(left, right, frames, cycle);
    // Not the audio thread, and run() is not running: free directly. A pending
    // slot has no loop; its late answer is handled in response().
    delete s.loop;
    s.loop = loop;
    s.failed = s.retiring = false;
    s.gain = Ramp{};
    s.enabled = true;
    share(n);
    return true;
}
void LoopSet::hold(uint32_t value) noexcept
{
    value = std::clamp(value, 1U, kMaxLoops);
    hold_ = value;
    held_seen_ = false;
    count_ = value;
    selected_ = std::min(selected_, count_ - 1);
    shared_count_.store(count_, std::memory_order_relaxed);
    for (uint32_t n = 0; n < kMaxLoops; ++n) { share(n); }
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
void LoopSet::global(const GlobalSettings& settings) noexcept
{
    global_ = settings;
    // Engine::set_tempo restarts the eighth-note counters on a tempo change.
    if (settings.bpm != counted_bpm_) { tempo_counter_ = quarter_counter_ = 0; counted_bpm_ = settings.bpm; }
}
void LoopSet::activate() noexcept
{
    tempo_counter_ = quarter_counter_ = 0;
    cycle_left_ = loop_left_ = 0;
}
int LoopSet::sync_loop() const noexcept
{
    // Engine::process runs the sync source loop first, when it exists.
    if (global_.sync_to < 2) { return -1; }
    const uint32_t n = global_.sync_to - 2;
    return n < count_ && slots_[n].loop && !slots_[n].retiring ? int(n) : -1;
}
void LoopSet::configure(uint32_t n, const LoopSettings& settings, bool enabled) noexcept
{
    auto& s = slots_[n];
    if (!s.loop) { return; }
    LoopSettings value = settings;
    value.quantize = global_.quantize;
    value.round = global_.round;
    value.mute_quantized = global_.mute_quantized;
    value.overdub_quantized = global_.overdub_quantized;
    value.replace_quantized = global_.replace_quantized;
    value.eighths = global_.eighths;
    // Engine::update_sync_source / calculate_tempo_frames: no tempo without a
    // sync source. Looper::run: the sync source loop itself runs free, and
    // Relative sync makes the core's Sync 2.
    value.tempo = global_.sync_to == 0 || global_.bpm <= 1 ? 0 : global_.bpm;
    const int source = sync_loop();
    // Adaptation: upstream gives an absent source an all-zero sync buffer, so
    // a synced loop would wait forever to record. Loo-pi runs it free.
    const bool has_source = global_.sync_to == 1 || source >= 0;
    if (!has_source || int(n) == source || settings.sync < .5f) { value.sync = 0; }
    else { value.sync = global_.relative_sync ? 2 : 1; }
    s.loop->configure(value); s.loop->enabled(enabled);
    if (enabled && !s.enabled && solo_ >= 0 && int(n) != solo_) { hit(n, Event::MUTE_ON); }
    s.enabled = enabled;
}
void LoopSet::generate_sync(uint32_t frames) noexcept
{
    // Engine::calculate_tempo_frames and Engine::generate_sync, internal tempo:
    // a 2.0 every eighth (Quantize 8th) or every cycle (Quantize Cycle); other
    // quantize modes have no tempo frames and get 1.0 everywhere.
    const double tempo = global_.bpm;
    double tempo_frames = 0, quarter_frames = 0;
    if (tempo > 0.0) {
        if (global_.quantize == QUANT_8TH) { tempo_frames = rate_ * (1.0 / tempo) * 30.0; }
        else if (global_.quantize == QUANT_CYCLE) { tempo_frames = rate_ * (1.0 / tempo) * 30.0 * global_.eighths; }
        quarter_frames = rate_ * (1.0 / tempo) * 60;
    }
    float* buffer = sync_.data();
    if (tempo_frames == 0.0) {
        double qcurr = quarter_counter_ + double(frames);
        if (qcurr >= quarter_frames) { qcurr = ((qcurr - quarter_frames) - std::trunc(qcurr - quarter_frames)) + 1.0; }
        quarter_counter_ = qcurr;
        std::fill_n(buffer, frames, 1.0f);
        return;
    }
    double curr = tempo_counter_, qcurr = quarter_counter_;
    uint32_t npos = 0;
    while (npos < frames) {
        while (curr < tempo_frames && qcurr < quarter_frames && npos < frames) {
            buffer[npos++] = 0.0f;
            curr += 1.0;
            qcurr += 1.0;
        }
        if (qcurr >= quarter_frames) { qcurr = ((qcurr - quarter_frames) - std::trunc(qcurr - quarter_frames)) + 1.0; }
        if (curr >= tempo_frames && npos < frames) {
            buffer[npos++] = 2.0f;
            curr = ((curr - tempo_frames) - std::trunc(curr - tempo_frames)) + 1.0;
        }
    }
    tempo_counter_ = curr;
    quarter_counter_ = qcurr;
}

void LoopSet::clocks(float* cycle_clock, float* loop_clock, uint32_t frames) noexcept
{
    // Continue pulses begun in an earlier slice.
    for (uint32_t i = 0; i < frames; ++i) {
        if (cycle_clock) { cycle_clock[i] = cycle_left_ ? 1.0f : 0.0f; }
        if (loop_clock) { loop_clock[i] = loop_left_ ? 1.0f : 0.0f; }
        if (cycle_left_) { --cycle_left_; }
        if (loop_left_) { --loop_left_; }
    }
    // The sync loop, or the selected loop when syncing to Tempo or None. The
    // loop reports the host frames that played a cycle or loop start.
    const int source = sync_loop();
    const StereoLoop* loop = slots_[source >= 0 ? uint32_t(source) : selected_].loop;
    if (!loop) { return; }
    const auto pulse = [&](float* out, uint32_t& left, uint32_t from) {
        const uint32_t end = std::min(frames, from + pulse_samples_);
        if (out) { std::fill(out + from, out + end, 1.0f); }
        left = from + pulse_samples_ - end;
    };
    const uint32_t* at = nullptr;
    for (uint32_t i = 0, n = loop->starts(false, at); i < n; ++i) { pulse(cycle_clock, cycle_left_, at[i]); }
    for (uint32_t i = 0, n = loop->starts(true, at); i < n; ++i) { pulse(loop_clock, loop_left_, at[i]); }
}

bool LoopSet::process(const float* l, const float* r, float* out_l, float* out_r, uint32_t frames,
                      float* cycle_clock, float* loop_clock) noexcept
{
    bool nonfinite = false;
    std::fill_n(out_l, frames, 0); std::fill_n(out_r, frames, 0);
    const float* sync = nullptr;
    if (global_.sync_to == 1) { generate_sync(frames); sync = sync_.data(); }
    const auto run = [&](Slot& s, const float* sync_in, float* sync_out) {
        s.loop->process(l, r, scratch_[0].data(), scratch_[1].data(), frames, sync_in, sync_out);
        bool bad = false;
        for (uint32_t i = 0; i < frames; ++i) { bad |= !std::isfinite(scratch_[0][i]) || !std::isfinite(scratch_[1][i]); }
        if (bad) { s.loop->recover(); }
        nonfinite |= bad;
        for (uint32_t i = 0; i < frames; ++i) {
            const auto gain = s.gain.next();
            if (!bad) { out_l[i] += scratch_[0][i] * gain; out_r[i] += scratch_[1][i] * gain; }
        }
    };
    // Engine::process: the sync source loop runs first; its channel 0 sync
    // output is every other loop's sync input.
    const int source = sync_loop();
    if (source >= 0) { run(slots_[source], nullptr, sync_.data()); sync = sync_.data(); }
    for (uint32_t n = 0; n < kMaxLoops; ++n) {
        if (int(n) != source && slots_[n].loop) { run(slots_[n], sync, nullptr); }
    }
    clocks(cycle_clock, loop_clock, frames);
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
