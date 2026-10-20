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
bool supported(Command command)
{
    switch (command) {
    case Event::RECORD: case Event::OVERDUB: case Event::RECORD_OR_OVERDUB:
    case Event::MUTE: case Event::UNDO: case Event::REDO: case Event::UNDO_ALL:
    case Event::REDO_ALL: case Event::TRIGGER: case Event::RECORD_OVERDUB_END_SOLO: return true;
    default: return false;
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
      ramp_samples_(std::max(1U, uint32_t(rate * 0.01))), rate_(rate)
{
    for (auto& buffer : scratch_) { buffer.resize(maximum_); }
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
            if (rec_pressed_ && frames_ - last_rec_press_ < uint64_t(rate_ * 0.5)) {
                requested = Event::UNDO_TWICE;
            }
            last_rec_press_ = frames_;
            rec_pressed_ = true;
        }
        down_[cmd] = frames_;
        held_[cmd] = true;
        enqueue(requested);
    } else {
        const bool long_press = held_[cmd] && frames_ - down_[cmd] > uint64_t(rate_);
        if (long_press && (output(State) != LooperStatePlaying || command == Event::UNDO || command == Event::REDO)) {
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
    uint32_t count = std::min(requested, maximum_);
    // Undo/Redo commit at the END of a core run. A one-sample run during their
    // fades makes the commit independent of host slicing, leaving steady audio
    // on the normal block path.
    if (transient()) { return 1; }
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

void StereoLoop::run_core(uint32_t frames, Command command, bool bypass) noexcept
{
    for (uint32_t channel = 0; channel < 2; ++channel) {
        auto& p = ports_[channel];
        auto* state = static_cast<SooperLooperI*>(cores_[channel]);
        p[Multi] = float(command);
        p[TriggerThreshold] = settings_.threshold;
        p[FadeSamples] = bypass ? float(ramp_samples_) : float(fade_samples_);
        p[UseSafetyFeedback] = settings_.safety_feedback ? 1 : 0;
        p[UseFeedbackPlay] = settings_.play_feedback ? 1 : 0;
        p[Feedback] = feedback_.current;
        // Scalar storage is private: the core is allowed to write Feedback.
        // Our fixed-duration ramp replaces its host-block-dependent ramp.
        state->fFeedbackCurr = feedback_.current;
        // Looper::run_loops: channel 1 consumes channel 0's sync output.
        connectPortToSooperLooper(cores_[channel], AudioInputPort, scratch_[channel].data());
        connectPortToSooperLooper(cores_[channel], AudioOutputPort, scratch_[2 + channel].data());
        connectPortToSooperLooper(cores_[channel], SyncInputPort, scratch_[channel ? 4 : 6].data());
        connectPortToSooperLooper(cores_[channel], SyncOutputPort, scratch_[channel ? 5 : 4].data());
        runSooperLooper(cores_[channel], frames);
    }
}

void StereoLoop::process(const float* left, const float* right, float* out_l, float* out_r, uint32_t frames) noexcept
{
    uint32_t offset = 0;
    while (offset < frames) {
        if (second_undo_ && !transient()) {
            enqueue(Event::UNDO);
            second_undo_ = false;
        }
        Request request;
        uint32_t count = safe_chunk(frames - offset);
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
        for (uint32_t i = 0; i < count; ++i) {
            const auto gain = input_gain_.next();
            const auto l = left ? left[offset + i] : 0;
            const auto r = right ? right[offset + i] : 0;
            scratch_[0][i] = std::isfinite(l) ? gain * l : 0;
            scratch_[1][i] = std::isfinite(r) ? gain * r : 0;
            scratch_[6][i] = 0;
        }
        run_core(count, request.command, request.bypass);
        for (uint32_t i = 0; i < count; ++i) {
            const float gain = level_.next() * gate_.next();
            if (out_l) { out_l[offset + i] = scratch_[2][i] * gain; }
            if (out_r) { out_r[offset + i] = scratch_[3][i] * gain; }
        }
        offset += count;
        frames_ += count;
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
