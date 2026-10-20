// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#include "rt_guard.hpp"
#include <cstddef>
#include <cstdlib>
#include <new>
namespace loopi::test {
thread_local bool audio_active = false;
thread_local uint64_t heap_calls = 0;
thread_local int fail_after = -1;
bool fail() {
    if (fail_after < 0) { return false; }
    if (fail_after == 0) { return true; }
    --fail_after; return false;
}
}
extern "C" {
void* __real_malloc(std::size_t);
void* __real_calloc(std::size_t, std::size_t);
void* __real_realloc(void*, std::size_t);
void __real_free(void*);
void* __wrap_malloc(std::size_t size)
{ if (loopi::test::audio_active) { ++loopi::test::heap_calls; } return loopi::test::fail() ? nullptr : __real_malloc(size); }
void* __wrap_calloc(std::size_t n, std::size_t size)
{ if (loopi::test::audio_active) { ++loopi::test::heap_calls; } return loopi::test::fail() ? nullptr : __real_calloc(n, size); }
void* __wrap_realloc(void* ptr, std::size_t size)
{ if (loopi::test::audio_active) { ++loopi::test::heap_calls; } return loopi::test::fail() ? nullptr : __real_realloc(ptr, size); }
void __wrap_free(void* ptr)
{ if (loopi::test::audio_active) { ++loopi::test::heap_calls; } __real_free(ptr); }
}
void* operator new(std::size_t size)
{ if (auto* p = std::malloc(size ? size : 1)) { return p; } throw std::bad_alloc(); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
