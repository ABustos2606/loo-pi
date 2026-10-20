// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#pragma once
#include <cstdint>
namespace loopi::test {
extern thread_local bool audio_active;
extern thread_local uint64_t heap_calls;
// Fault injection on the non-audio worker, -1 disables it.
extern thread_local int fail_after;
struct AudioGuard {
    AudioGuard() { audio_active = true; }
    ~AudioGuard() { audio_active = false; }
};
}
