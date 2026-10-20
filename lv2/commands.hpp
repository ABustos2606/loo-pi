// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#pragma once
#include "ports.hpp"
#include "stereo_loop.hpp"
#include "loop_set.hpp"
#include <lv2/atom/atom.h>
#include <array>
#include <cmath>
#include <cstring>

namespace loopi {
class CommandEvents {
public:
    CommandEvents(const void* data, uint32_t sequence_type, uint32_t int_type, uint32_t frame_type, uint32_t frames) noexcept
        : int_type_(int_type), frames_(frames)
    {
        if (!data) { return; }
        LV2_Atom_Sequence sequence;
        std::memcpy(&sequence, data, sizeof(sequence));
        if (sequence.atom.type != sequence_type || sequence.atom.size < sizeof(sequence.body) ||
            sequence.atom.size > 1048576 || (sequence.body.unit && sequence.body.unit != frame_type)) { return; }
        cursor_ = static_cast<const uint8_t*>(data) + sizeof(sequence);
        remaining_ = sequence.atom.size - sizeof(sequence.body);
    }
    bool next(uint32_t& frame, uint32_t& target, uint32_t& command, bool& press) noexcept
    {
        while (remaining_ >= sizeof(LV2_Atom_Event)) {
            LV2_Atom_Event event;
            std::memcpy(&event, cursor_, sizeof(event));
            const uint64_t size = sizeof(event) + ((uint64_t(event.body.size) + 7) & ~uint64_t(7));
            if (size > remaining_) { remaining_ = 0; return false; }
            const auto* body = cursor_ + sizeof(event);
            cursor_ += size;
            remaining_ -= uint32_t(size);
            if (event.body.type != int_type_ || event.body.size != sizeof(int32_t) ||
                event.time.frames < 0 || uint64_t(event.time.frames) >= frames_) { continue; }
            int32_t value;
            std::memcpy(&value, body, sizeof(value));
            if (value < 0 || value / 256 > int32_t(kMaxLoops + 1)) { continue; }
            const int32_t number = (value % 256) / 2;
            if (number >= SooperLooper::Event::LAST_COMMAND && number != int32_t(kNextLoop) && number != int32_t(kPreviousLoop)) { continue; }
            frame = uint32_t(event.time.frames);
            target = uint32_t(value / 256);
            command = uint32_t(number);
            press = (value & 1) != 0;
            return true;
        }
        return false;
    }
private:
    const uint8_t* cursor_ = nullptr;
    uint32_t remaining_ = 0, int_type_, frames_;
};

class CommandControls {
public:
    void update(const std::array<void*, index(Port::count)>& ports, LoopSet& loop) noexcept
    {
        constexpr std::array<Port, 7> inputs{Port::rec_dub, Port::overdub, Port::undo, Port::mute, Port::trigger,
            Port::pause, Port::next_loop};
        constexpr std::array<uint32_t, 7> commands{SooperLooper::Event::RECORD_OR_OVERDUB,
            SooperLooper::Event::OVERDUB, SooperLooper::Event::UNDO,
            SooperLooper::Event::MUTE, SooperLooper::Event::TRIGGER, SooperLooper::Event::PAUSE, kNextLoop};
        for (uint32_t i = 0; i < inputs.size(); ++i) {
            const auto* input = static_cast<const float*>(ports[index(inputs[i])]);
            const bool down = input && std::isfinite(*input) && *input >= 0.5f;
            if (down != previous_[i]) { loop.event(0, commands[i], down); previous_[i] = down; }
        }
    }
private:
    std::array<bool, 7> previous_{};
};
} // namespace loopi
