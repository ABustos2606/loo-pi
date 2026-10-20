// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
// Copyright (C) 2004 Jesse Chappell <jesse@essej.net>
#include "stereo_loop.hpp"
#include <algorithm>
#include <cmath>
#include <new>
#include <sys/mman.h>

extern void activateSooperLooper(LADSPA_Handle);
extern void cleanupSooperLooper(LADSPA_Handle);
extern void connectPortToSooperLooper(LADSPA_Handle, unsigned long, LADSPA_Data*);
extern void runSooperLooper(LADSPA_Handle, unsigned long);

namespace loopi {
using namespace SooperLooper;
namespace {
// Feedback ramps in steps on absolute-frame boundaries. Within a step the
// core sees constant feedback, so splitting a step cannot change its output.
constexpr uint32_t kFeedbackStep = 32;
void touch(float* data, unsigned long count)
{
    volatile float* memory = data;
    for (unsigned long i = 0; i < count; i += 1024) { memory[i] = 0; }
    if (count) { memory[count - 1] = 0; }
}
void destroy(LADSPA_Handle core)
{
    if (!core) { return; }
    auto* state = static_cast<SooperLooperI*>(core);
    (void)munlock(state->pSampleBuf, state->lBufferSize * sizeof(float));
    (void)munlock(state->pInputBuf, state->lInputBufSize * sizeof(float));
    cleanupSooperLooper(core);
}
// Every per-loop command the core implements. Not here: Scratch (no scratch
// position control), the mute-mode toggles that would rewrite our quantize and
// redo settings, and the Engine-level Solo/exclusive commands, which LoopSet
// translates before they reach a loop (Engine::do_global_rt_event).
bool supported(Command command)
{
    switch (command) {
    case Event::UNKNOWN: case Event::SCRATCH: case Event::REDO_TOG: case Event::QUANT_TOG: case Event::ROUND_TOG:
    case Event::SOLO: case Event::SOLO_NEXT: case Event::SOLO_PREV:
    case Event::RECORD_SOLO: case Event::RECORD_SOLO_NEXT: case Event::RECORD_SOLO_PREV:
    case Event::RECORD_EXCLUSIVE: case Event::RECORD_EXCLUSIVE_NEXT: case Event::RECORD_EXCLUSIVE_PREV:
    case Event::RECORD_OR_OVERDUB_EXCL: case Event::RECORD_OR_OVERDUB_EXCL_NEXT: case Event::RECORD_OR_OVERDUB_EXCL_PREV:
    case Event::RECORD_OR_OVERDUB_SOLO: case Event::RECORD_OR_OVERDUB_SOLO_TRIG: case Event::RECORD_OVERDUB_END_SOLO_TRIG:
    case Event::RECORD_OR_OVERDUB_SOLO_NEXT: case Event::RECORD_OR_OVERDUB_SOLO_PREV: return false;
    default: return command >= 0 && command < Event::LAST_COMMAND;
    }
}
}

void Ramp::set(float value, uint32_t samples) noexcept
{
    if (value == target) { return; }
    target = value;
    remaining = samples;
    step = samples ? (target - current) / samples : 0;
    if (!samples) { current = target; }
}
float Ramp::next() noexcept
{
    if (remaining) {
        current += step;
        if (!--remaining) { current = target; }
    }
    return current;
}

StereoLoop::StereoLoop(double rate, uint32_t maximum, float seconds)
    : maximum_(std::clamp(maximum, 1U, 8192U)),
      // Rate up to 4 runs the cores four frames per host frame.
      core_maximum_(4 * maximum_ + 8), ramp_samples_(std::max(1U, uint32_t(rate * 0.01))), rate_(rate)
{
    for (auto& buffer : scratch_) { buffer.resize(core_maximum_); }
    emits_.resize(maximum_);
    // Looper::initialize: separate core instances, scalar ports before activate,
    // slave Sync=1. No shared LADSPA descriptor, environment, or process state.
    for (uint32_t channel = 0; channel < 2; ++channel) {
        cores_[channel] = sl_instantiate(static_cast<unsigned long>(rate), seconds);
        if (!cores_[channel]) {
            for (auto core : cores_) { destroy(core); }
            throw std::bad_alloc();
        }
        auto* state = static_cast<SooperLooperI*>(cores_[channel]);
        touch(state->pSampleBuf, state->lBufferSize);
        touch(state->pInputBuf, state->lInputBufSize);
        // Best effort only: hosts commonly have a small RLIMIT_MEMLOCK.
        (void)mlock(state->pSampleBuf, state->lBufferSize * sizeof(float));
        (void)mlock(state->pInputBuf, state->lInputBufSize * sizeof(float));
        auto& values = ports_[channel];
        values[WetLevel] = values[Feedback] = values[Rate] = 1;
        values[Multi] = -1;
        values[FadeSamples] = 64;
        values[TempoInput] = 120;
        values[EighthPerCycleLoop] = 16;
        values[UseSafetyFeedback] = 1;
        values[Sync] = channel ? 1 : 0;
        // Latencies, ScratchPosition, TapDelayTrigger, RedoTap,
        // RoundIntegerTempo and SyncOffsetSamples stay zero.
        for (uint32_t p = 0; p < values.size(); ++p) {
            connectPortToSooperLooper(cores_[channel], p, &values[p]);
        }
        activateSooperLooper(cores_[channel]);
    }
}
StereoLoop::~StereoLoop()
{
    for (auto core : cores_) { destroy(core); }
}

void StereoLoop::finish_state() noexcept
{
    // Looper::finish_state: end a currently active editing operation.
    if (output(State) == LooperStateRecording) { enqueue(Event::RECORD); }
    else if (output(State) == LooperStateOverdubbing) { enqueue(Event::OVERDUB); }
}

void StereoLoop::configure(const LoopSettings& value) noexcept
{
    settings_ = value;
    // Looper::set_replace_quantized; the other quantize options are ports.
    for (auto core : cores_) { sl_set_replace_quantized(core, value.replace_quantized); }
    const double speed = std::isfinite(value.rate) ? std::clamp(double(value.rate), 0.25, 4.0) : 1.0;
    // Keep the phase of the next loop-time frame when the speed changes, so the
    // output read position keeps its distance behind the input (resampled()).
    if (resampling_ && speed != rate_value_ && speed != 1) { in_phase_ *= rate_value_ / speed; }
    rate_value_ = speed;
    input_gain_.set(value.input_gain, configured_ ? ramp_samples_ : 0);
    level_.set(value.level, configured_ ? ramp_samples_ : 0);
    feedback_.set(value.feedback, configured_ ? (ramp_samples_ + kFeedbackStep - 1) / kFeedbackStep : 0);
    configured_ = true;
    fade_samples_ = std::max(1U, uint32_t(value.fade_ms * rate_ / 1000));
}

void StereoLoop::enqueue(Command command, bool bypass) noexcept
{
    // Keep a bounded sequence, including simultaneous stomp/atom requests,
    // rather than overwriting Looper's single requested_cmd slot.
    if (size_ < requests_.size()) {
        requests_[(head_ + size_) % requests_.size()] = {command, bypass};
        ++size_;
    }
}

void StereoLoop::event(Command command, bool press) noexcept
{
    if (!supported(command)) { return; }
    const auto cmd = static_cast<uint32_t>(command);
    // Looper::do_event, type_cmd_down/type_cmd_up: press acts, a release after
    // >1 s acts again unless already Playing; Undo/Redo have their -all forms.
    if (press) {
        auto requested = command;
        if (command == Event::RECORD_OR_OVERDUB) {
            if (rec_pressed_ && host_frames_ - last_rec_press_ < uint64_t(rate_ * 0.5)) {
                requested = Event::UNDO_TWICE;
            }
            last_rec_press_ = host_frames_;
            rec_pressed_ = true;
        }
        down_[cmd] = host_frames_;
        held_[cmd] = true;
        enqueue(requested);
    } else {
        const bool long_press = held_[cmd] && host_frames_ - down_[cmd] > uint64_t(rate_);
        if (long_press && (output(State) != LooperStatePlaying || command == Event::UNDO || command == Event::REDO ||
                           command == Event::REVERSE || command == Event::DELAY)) {
            auto requested = command;
            if (command == Event::UNDO || command == Event::RECORD_OR_OVERDUB || command == Event::RECORD_OVERDUB_END_SOLO) { requested = Event::UNDO_ALL; }
            else if (command == Event::REDO) { requested = Event::REDO_ALL; }
            enqueue(requested);
        }
        held_[cmd] = false;
    }
    // Distinct held/last-press fields retain the rule at frame zero and across
    // a normal press/release pair (upstream's zero sentinel loses those cases).
}

void StereoLoop::enabled(bool value) noexcept
{
    if (value == enabled_) { return; }
    enabled_ = value;
    gate_.set(value ? 1 : 0, ramp_samples_);
    if (!value) {
        restore_ = output(State) != LooperStateMuted && output(State) != LooperStateOffMuted;
        enqueue(Event::MUTE_ON, true);
    } else if (restore_) {
        enqueue(Event::MUTE_OFF, true);
        restore_ = false;
    }
}

bool StereoLoop::transient() const noexcept
{
    const auto state = output(State);
    return state == LooperStateUndo || state == LooperStateRedo ||
           state == LooperStateUndoAll || state == LooperStateRedoAll;
}
uint32_t StereoLoop::safe_chunk(uint32_t requested) const noexcept
{
    uint32_t count = std::min(requested, core_maximum_);
    // Undo/Redo commit at the END of a core run. A one-sample run during their
    // fades makes the commit independent of host slicing, leaving steady audio
    // on the normal block path.
    if (transient()) { return 1; }
    // A quantized Reverse commits pLS->fCurrRate on the boundary sample, but
    // the run keeps its local rate until it ends, so the direction would flip
    // wherever the host happened to end the run. One-sample runs while it is
    // pending make the flip land on the frame after the boundary.
    for (auto core : cores_) {
        if (static_cast<const SooperLooperI*>(core)->fNextCurrRate != 0) { return 1; }
    }
    // A changing feedback never crosses a step boundary (see kFeedbackStep).
    if (feedback_.remaining) { count = std::min(count, kFeedbackStep - uint32_t(frames_ % kFeedbackStep)); }
    for (auto core : cores_) {
        const auto* state = static_cast<const SooperLooperI*>(core);
        if (state->state == LooperStateRecording && state->headLoopChunk) {
            const auto left = state->lBufferSize - state->headLoopChunk->lLoopLength;
            count = std::min(count, uint32_t(std::max(1UL, left)));
        }
    }
    return count;
}

void StereoLoop::run_core(uint32_t frames, Command command, bool bypass, uint32_t at) noexcept
{
    for (uint32_t channel = 0; channel < 2; ++channel) {
        auto& p = ports_[channel];
        auto* state = static_cast<SooperLooperI*>(cores_[channel]);
        p[Multi] = float(command);
        p[TriggerThreshold] = settings_.threshold;
        // Looper shares one port array between channels; only Sync differs:
        // the slave follows channel 0 with 1, or 2 in relative mode
        // (Looper::run, _slave_sync_port).
        p[Quantize] = settings_.quantize;
        p[Round] = settings_.round ? 1 : 0;
        p[MuteQuantized] = settings_.mute_quantized ? 1 : 0;
        p[OverdubQuantized] = settings_.overdub_quantized ? 1 : 0;
        p[Sync] = channel ? (settings_.sync == 2 ? 2 : 1) : settings_.sync;
        p[PlaybackSync] = settings_.play_sync ? 1 : 0;
        p[TempoInput] = settings_.tempo;
        p[EighthPerCycleLoop] = settings_.eighths;
        p[FadeSamples] = bypass ? float(ramp_samples_) : float(fade_samples_);
        p[UseSafetyFeedback] = settings_.safety_feedback ? 1 : 0;
        p[UseFeedbackPlay] = settings_.play_feedback ? 1 : 0;
        p[Feedback] = feedback_.current;
        // Scalar storage is private: the core is allowed to write Feedback.
        // Our fixed-duration ramp replaces its host-block-dependent ramp.
        state->fFeedbackCurr = feedback_.current;
        // Looper::run_loops: channel 1 consumes channel 0's sync output.
        connectPortToSooperLooper(cores_[channel], AudioInputPort, scratch_[channel].data() + at);
        connectPortToSooperLooper(cores_[channel], AudioOutputPort, scratch_[2 + channel].data() + at);
        connectPortToSooperLooper(cores_[channel], SyncInputPort, scratch_[channel ? 4 : 6].data() + at);
        connectPortToSooperLooper(cores_[channel], SyncOutputPort, scratch_[channel ? 5 : 4].data() + at);
        runSooperLooper(cores_[channel], frames);
    }
}

void StereoLoop::run_block(uint32_t frames) noexcept
{
    uint32_t at = 0;
    while (at < frames) {
        if (second_undo_ && !transient()) {
            enqueue(Event::UNDO);
            second_undo_ = false;
        }
        Request request;
        uint32_t count = safe_chunk(frames - at);
        if (size_) {
            count = 1;
            const auto next = requests_[head_];
            // Looper::run: identical consecutive Multi values need an idle
            // run between them. Defer by ONE SAMPLE, never by a host block.
            if (ports_[0][Multi] != float(next.command)) {
                request = next;
                head_ = (head_ + 1) % requests_.size();
                --size_;
                if (request.command == Event::UNDO_TWICE) {
                    request.command = Event::UNDO;
                    second_undo_ = true;
                }
            }
        }
        // While ramping, each boundary starts exactly one run: advance once.
        if (frames_ % kFeedbackStep == 0) { feedback_.next(); }
        run_core(count, request.command, request.bypass, at);
        at += count;
        frames_ += count;
        track(count);
    }
}

void StereoLoop::boundary(uint64_t frame, bool loop) noexcept
{ if (pending_count_ < pending_.size()) { pending_[pending_count_++] = {frame, loop}; } }

void StereoLoop::track(uint32_t ran) noexcept
{
    // The core moves its position by exactly one frame per frame (+1, or -1
    // reversed) except where a command jumps it. So the boundaries a run
    // passed, and the frame that played each one, follow from the position
    // before the run, the frames run and the position after; a single change
    // of direction inside the run (a quantized Reverse) can be solved for.
    // Independent of how the host slices the period.
    const auto* core = static_cast<const SooperLooperI*>(cores_[0]);
    const auto* chunk = core->headLoopChunk;
    const int state = int(ports_[0][State]);
    const double len = chunk ? double(chunk->lLoopLength) : 0, cyc = chunk ? double(chunk->lCycleLength) : 0;
    const bool running = len > 0 && cyc > 0 &&
        (state == LooperStatePlaying || state == LooperStateOverdubbing || state == LooperStateMultiplying ||
         state == LooperStateInserting || state == LooperStateReplacing || state == LooperStateSubstitute ||
         state == LooperStateMuted || state == LooperStateOneShot || state == LooperStateTriggerPlay);
    const double pos = chunk ? chunk->dCurrPos : 0, d = ran;
    const bool back = core->fCurrRate < 0;
    const uint64_t end = frames_, start = end - ran;
    const auto restart = [&] { // the frame that played position 0
        if (!back && pos <= d) { boundary(end - uint64_t(pos), false); boundary(end - uint64_t(pos), true); }
    };
    // Boundaries b passed going up from `from` to `to`, (from, to], or down,
    // [to, from), with `at` the frame that played `from`.
    const auto up = [&](double from, double to, uint64_t at) {
        for (double b = (std::floor(from / cyc) + 1) * cyc, n = 0; b <= to && n < 16; b += cyc, ++n) { boundary(at + uint64_t(b - from), false); }
        for (double b = (std::floor(from / len) + 1) * len, n = 0; b <= to && n < 16; b += len, ++n) { boundary(at + uint64_t(b - from), true); }
    };
    const auto down = [&](double from, double to, uint64_t at) {
        for (double b = (std::ceil(from / cyc) - 1) * cyc, n = 0; b >= to && n < 16; b -= cyc, ++n) { boundary(at + uint64_t(from - b), false); }
        for (double b = (std::ceil(from / len) - 1) * len, n = 0; b >= to && n < 16; b -= len, ++n) { boundary(at + uint64_t(from - b), true); }
    };
    if (running && !track_valid_) { restart(); } // a recording closing into Play
    else if (running && back == track_back_) {
        const double to = back ? track_pos_ - d : track_pos_ + d;
        double wrapped = std::fmod(to, len);
        if (wrapped < 0) { wrapped += len; }
        if (std::abs(wrapped - pos) < .5) { back ? down(track_pos_, to, start) : up(track_pos_, to, start); }
        else { restart(); } // a jump: Trigger, Undo, a new length
    } else if (running) {
        // Reversed at r inside the run: forwards f frames then backwards d - f
        // gives pos = 2r - from - d (or, backwards first, pos = 2r - from + d).
        for (int k = -1; k <= 2; ++k) {
            const double end_pos = pos + k * len;
            const double r = (end_pos + track_pos_ + (track_back_ ? -d : d)) / 2;
            if (!track_back_ && r >= track_pos_ && r <= track_pos_ + d) {
                up(track_pos_, r, start); down(r, end_pos, start + uint64_t(r - track_pos_)); break;
            }
            if (track_back_ && r <= track_pos_ && r >= track_pos_ - d) {
                down(track_pos_, r, start); up(r, end_pos, start + uint64_t(track_pos_ - r)); break;
            }
        }
    }
    track_valid_ = running;
    track_pos_ = pos;
    track_back_ = back;
}

void StereoLoop::place(uint64_t block_start, uint32_t core_frames, uint32_t host_offset, bool resampled) noexcept
{
    // Move the boundaries played in this block to host-frame offsets; keep
    // those still to be played (a boundary on the frame after the block).
    uint32_t kept = 0;
    for (uint32_t e = 0; e < pending_count_; ++e) {
        const auto event = pending_[e];
        if (event.frame >= block_start + core_frames) { pending_[kept++] = event; continue; }
        const uint64_t rel = event.frame >= block_start ? event.frame - block_start : 0;
        uint32_t host = uint32_t(rel);
        if (resampled) { host = 0; while (emits_[host] <= rel) { ++host; } }
        auto& at = event.loop ? loop_at_ : cycle_at_;
        auto& count = event.loop ? loop_count_ : cycle_count_;
        if (count < at.size()) { at[count++] = host_offset + host; }
    }
    pending_count_ = kept;
}

float StereoLoop::history(uint32_t channel, uint64_t index, uint64_t block_start) const noexcept
{
    if (index >= block_start) { return scratch_[2 + channel][index - block_start]; }
    const uint64_t back = block_start - index;
    return back <= tail_[channel].size() ? tail_[channel][tail_[channel].size() - back] : tail_[channel][0];
}

void StereoLoop::keep_tail(uint64_t block_start) noexcept
{
    std::array<std::array<float, 8>, 2> tail;
    for (uint32_t c = 0; c < 2; ++c) {
        for (uint32_t j = 0; j < 8; ++j) {
            const uint64_t back = 8 - j;
            tail[c][j] = frames_ >= back ? history(c, frames_ - back, block_start) : 0;
        }
    }
    tail_ = tail;
}

void StereoLoop::direct(const float* left, const float* right, float* out_l, float* out_r, uint32_t frames,
                        const float* sync_in, float* sync_out) noexcept
{
    for (uint32_t i = 0; i < frames; ++i) {
        const auto gain = input_gain_.next();
        const auto l = left ? left[i] : 0;
        const auto r = right ? right[i] : 0;
        scratch_[0][i] = std::isfinite(l) ? gain * l : 0;
        scratch_[1][i] = std::isfinite(r) ? gain * r : 0;
        scratch_[6][i] = sync_in ? sync_in[i] : 0;
    }
    const uint64_t block_start = frames_;
    run_block(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        const float gain = level_.next() * gate_.next();
        if (out_l) { out_l[i] = scratch_[2][i] * gain; }
        if (out_r) { out_r[i] = scratch_[3][i] * gain; }
        if (sync_out) { sync_out[i] = scratch_[4][i]; }
    }
    last_in_ = {scratch_[0][frames - 1], scratch_[1][frames - 1]};
    keep_tail(block_start);
}

void StereoLoop::resampled(const float* left, const float* right, float* out_l, float* out_r, uint32_t frames,
                           const float* sync_in, float* sync_out) noexcept
{
    // Looper::run_loops, resampled branch: input (and sync) into loop time at
    // Rate, run the cores, output back into host time at 1/Rate; linear, as
    // SRC_LINEAR. Each loop-time frame lies at in_phase_ within the interval
    // from the previous host sample to the current one. A sync value moves to
    // the next loop-time frame whole, never interpolated, so no pulse is lost
    // or split (an adaptation: interpolating a one-sample pulse can drop it).
    const double step = 1.0 / rate_value_;
    uint32_t m = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        const auto gain = input_gain_.next();
        const auto l = left ? left[i] : 0;
        const auto r = right ? right[i] : 0;
        const float x0 = std::isfinite(l) ? gain * l : 0, x1 = std::isfinite(r) ? gain * r : 0;
        sync_pending_ = std::max(sync_pending_, sync_in ? sync_in[i] : 0.0f);
        while (in_phase_ < 1) {
            const auto t = float(in_phase_);
            scratch_[0][m] = last_in_[0] + t * (x0 - last_in_[0]);
            scratch_[1][m] = last_in_[1] + t * (x1 - last_in_[1]);
            scratch_[6][m] = sync_pending_;
            sync_pending_ = 0;
            ++m;
            in_phase_ += step;
        }
        in_phase_ -= 1;
        emits_[i] = m;
        last_in_ = {x0, x1};
    }
    const uint64_t block_start = frames_;
    run_block(m);
    uint32_t first = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        // The read position trails the written loop-time frames by about three
        // frames (set on entry), so both neighbours already exist.
        out_pos_ += rate_value_;
        const double floor_pos = std::floor(out_pos_);
        auto k = uint64_t(std::max(0.0, floor_pos));
        if (k + 2 > frames_) { k = frames_ >= 2 ? frames_ - 2 : 0; }
        const auto frac = float(out_pos_ - floor_pos);
        const float gain = level_.next() * gate_.next();
        const float a0 = history(0, k, block_start), b0 = history(0, k + 1, block_start);
        const float a1 = history(1, k, block_start), b1 = history(1, k + 1, block_start);
        if (out_l) { out_l[i] = (a0 + frac * (b0 - a0)) * gain; }
        if (out_r) { out_r[i] = (a1 + frac * (b1 - a1)) * gain; }
        if (sync_out) {
            float pulse = 0;
            for (uint32_t j = first; j < emits_[i]; ++j) { pulse = std::max(pulse, scratch_[4][j]); }
            sync_out[i] = pulse;
        }
        first = emits_[i];
    }
    keep_tail(block_start);
}

void StereoLoop::process(const float* left, const float* right, float* out_l, float* out_r, uint32_t frames,
                         const float* sync_in, float* sync_out) noexcept
{
    uint32_t offset = 0;
    cycle_count_ = loop_count_ = 0;
    while (offset < frames) {
        const uint32_t count = std::min(frames - offset, maximum_);
        const auto shift = [offset](auto* p) { return p ? p + offset : p; };
        const uint64_t block_start = frames_;
        if (rate_value_ != 1) {
            if (!resampling_) {
                // Enter at the current frame: the next loop-time frame is due
                // now, and reading starts three frames behind it.
                resampling_ = true;
                in_phase_ = 0;
                sync_pending_ = 0;
                out_pos_ = double(frames_) - 3;
            }
            resampled(shift(left), shift(right), shift(out_l), shift(out_r), count, shift(sync_in), shift(sync_out));
        } else {
            resampling_ = false;
            direct(shift(left), shift(right), shift(out_l), shift(out_r), count, shift(sync_in), shift(sync_out));
        }
        place(block_start, uint32_t(frames_ - block_start), offset, resampling_);
        offset += count;
        host_frames_ += count;
    }
}

float StereoLoop::output(OutputPort port) const noexcept
{
    if (port == LoopFreeMemory) {
        const auto* state = static_cast<const SooperLooperI*>(cores_[0]);
        const auto used = state->headLoopChunk ? state->headLoopChunk->lLoopLength : 0;
        return float(state->lBufferSize - used) / state->fSampleRate;
    }
    return ports_[0][port];
}
double StereoLoop::position() const noexcept
{
    const auto* state = static_cast<const SooperLooperI*>(cores_[0]);
    return state->headLoopChunk ? state->headLoopChunk->dCurrPos : 0;
}
double StereoLoop::length() const noexcept
{
    const auto* state = static_cast<const SooperLooperI*>(cores_[0]);
    return state->headLoopChunk ? double(state->headLoopChunk->lLoopLength) : 0;
}
double StereoLoop::cycle() const noexcept
{
    const auto* state = static_cast<const SooperLooperI*>(cores_[0]);
    return state->headLoopChunk ? double(state->headLoopChunk->lCycleLength) : 0;
}
bool StereoLoop::reversed() const noexcept
{ return static_cast<const SooperLooperI*>(cores_[0])->fCurrRate < 0; }
bool StereoLoop::stereo_locked() const noexcept
{
    const auto* l = static_cast<const SooperLooperI*>(cores_[0]);
    const auto* r = static_cast<const SooperLooperI*>(cores_[1]);
    if (l->state != r->state || bool(l->headLoopChunk) != bool(r->headLoopChunk)) { return false; }
    return !l->headLoopChunk || (l->headLoopChunk->lLoopLength == r->headLoopChunk->lLoopLength &&
        l->headLoopChunk->dCurrPos == r->headLoopChunk->dCurrPos);
}
void StereoLoop::recover() noexcept
{
    // Logical Clear: the core fades and moves history pointers, never clears
    // the audio allocation. Called after a nonfinite loop-output slice.
    head_ = size_ = 0;
    second_undo_ = false;
    enqueue(Event::UNDO_ALL);
}
} // namespace loopi
