// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#include "loop_state.hpp"
#include <lv2/atom/atom.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

namespace loopi {
namespace {
constexpr uint32_t kFlags = LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE;
// The longest loop memory: 300 s rounded up to a power of two at 48 kHz.
constexpr double kMaxSeconds = 600;

void put16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
void put32(uint8_t* p, uint32_t v) { put16(p, v & 0xffff); put16(p + 2, v >> 16); }
uint32_t get16(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
uint32_t get32(const uint8_t* p) { return get16(p) | get16(p + 2) << 16; }

struct File {
    FILE* f;
    explicit File(const char* path, const char* mode) : f(std::fopen(path, mode)) {}
    ~File() { if (f) { std::fclose(f); } }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
};

template <typename T>
const T* find(const LV2_Feature* const* features, const char* uri)
{
    for (auto f = features; f && *f; ++f) {
        if ((*f)->URI && !std::strcmp((*f)->URI, uri)) { return static_cast<const T*>((*f)->data); }
    }
    return nullptr;
}

// Paths from mapPath and makePath are freed with freePath when the host has it.
struct Paths {
    const LV2_State_Free_Path* free_path;
    void release(char* path) const
    {
        if (!path) { return; }
        if (free_path && free_path->free_path) { free_path->free_path(free_path->handle, path); }
        else { std::free(path); }
    }
};
} // namespace

StateKeys::StateKeys(const LV2_URID_Map& map)
{
    const std::string base = "https://github.com/ABustos2606/loo-pi#";
    const auto id = [&](const std::string& name) { return map.map(map.handle, (base + name).c_str()); };
    loops = id("loops");
    for (uint32_t n = 0; n < kMaxLoops; ++n) {
        const auto loop = "loop" + std::to_string(n + 1);
        file[n] = id(loop);
        cycle[n] = id(loop + "_cycle");
        memory[n] = id(loop + "_memory");
    }
    path = map.map(map.handle, LV2_ATOM__Path);
    int_type = map.map(map.handle, LV2_ATOM__Int);
    long_type = map.map(map.handle, LV2_ATOM__Long);
    float_type = map.map(map.handle, LV2_ATOM__Float);
}

bool write_wav(const char* path, const std::vector<float>& interleaved, uint32_t rate)
{
    File file(path, "wb");
    if (!file.f) { return false; }
    const uint64_t bytes = uint64_t(interleaved.size()) * 4;
    if (bytes > 0xffffffffULL - 58) { return false; }
    // WAVE_FORMAT_IEEE_FLOAT: an 18-byte fmt chunk and a fact chunk.
    uint8_t header[58];
    std::memcpy(header, "RIFF", 4); put32(header + 4, uint32_t(50 + bytes));
    std::memcpy(header + 8, "WAVEfmt ", 8); put32(header + 16, 18);
    put16(header + 20, 3); put16(header + 22, 2); put32(header + 24, rate); put32(header + 28, rate * 8);
    put16(header + 32, 8); put16(header + 34, 32); put16(header + 36, 0);
    std::memcpy(header + 38, "fact", 4); put32(header + 42, 4); put32(header + 46, uint32_t(interleaved.size() / 2));
    std::memcpy(header + 50, "data", 4); put32(header + 54, uint32_t(bytes));
    if (std::fwrite(header, 1, sizeof(header), file.f) != sizeof(header)) { return false; }
    std::vector<uint8_t> block(4 * 8192);
    for (size_t done = 0; done < interleaved.size();) {
        const size_t count = std::min<size_t>(interleaved.size() - done, 8192);
        for (size_t i = 0; i < count; ++i) {
            uint32_t bits;
            std::memcpy(&bits, &interleaved[done + i], 4);
            put32(block.data() + 4 * i, bits);
        }
        if (std::fwrite(block.data(), 4, count, file.f) != count) { return false; }
        done += count;
    }
    const bool flushed = std::fflush(file.f) == 0;
    return flushed && std::fclose(std::exchange(file.f, nullptr)) == 0;
}

bool read_wav(const char* path, std::vector<float>& left, std::vector<float>& right, uint32_t& rate)
{
    File file(path, "rb");
    if (!file.f) { return false; }
    uint8_t riff[12];
    if (std::fread(riff, 1, 12, file.f) != 12 || std::memcmp(riff, "RIFF", 4) || std::memcmp(riff + 8, "WAVE", 4)) { return false; }
    uint32_t format = 0, channels = 0, bits = 0, align = 0;
    rate = 0;
    for (;;) {
        uint8_t chunk[8];
        if (std::fread(chunk, 1, 8, file.f) != 8) { return false; }
        const uint32_t size = get32(chunk + 4);
        if (!std::memcmp(chunk, "fmt ", 4)) {
            uint8_t fmt[40]{};
            if (size < 16 || std::fread(fmt, 1, std::min<uint32_t>(size, 40), file.f) != std::min<uint32_t>(size, 40)) { return false; }
            if (size > 40 && std::fseek(file.f, long(size - 40), SEEK_CUR)) { return false; }
            format = get16(fmt);
            channels = get16(fmt + 2);
            rate = get32(fmt + 4);
            align = get16(fmt + 12);
            bits = get16(fmt + 14);
            // WAVE_FORMAT_EXTENSIBLE: the sub-format GUID starts with the tag.
            if (format == 0xfffe && size >= 26) { format = get16(fmt + 24); }
        } else if (!std::memcmp(chunk, "data", 4)) {
            const bool pcm = format == 1 && (bits == 16 || bits == 24 || bits == 32);
            const bool ieee = format == 3 && bits == 32;
            if (!(pcm || ieee) || !channels || align != channels * bits / 8 || !rate) { return false; }
            const uint64_t frames = size / align;
            if (frames > uint64_t(kMaxSeconds * rate)) { return false; }
            left.resize(frames);
            right.resize(frames);
            std::vector<uint8_t> block(size_t(align) * 4096);
            for (uint64_t done = 0; done < frames;) {
                const auto count = size_t(std::min<uint64_t>(frames - done, 4096));
                const auto got = std::fread(block.data(), align, count, file.f);
                for (size_t i = 0; i < got; ++i) {
                    float value[2]{};
                    for (uint32_t c = 0; c < std::min(channels, 2U); ++c) {
                        const uint8_t* p = block.data() + i * align + c * bits / 8;
                        float v;
                        if (ieee) { const uint32_t raw = get32(p); std::memcpy(&v, &raw, 4); }
                        else if (bits == 16) { v = float(int16_t(get16(p))) / 32768.0f; }
                        else if (bits == 24) { v = float(int32_t(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24) >> 8) / 8388608.0f; }
                        else { v = float(double(int32_t(get32(p))) / 2147483648.0); }
                        value[c] = std::isfinite(v) ? v : 0;
                    }
                    left[done + i] = value[0];
                    right[done + i] = channels == 1 ? value[0] : value[1];
                }
                if (got != count) { left.resize(done + got); right.resize(done + got); break; }
                done += count;
            }
            return true;
        } else if (std::fseek(file.f, long(size + (size & 1)), SEEK_CUR)) {
            return false;
        }
    }
}

LV2_State_Status save_state(LoopSet& loops, const StateKeys& keys, double rate, LV2_State_Store_Function store,
                            LV2_State_Handle handle, const LV2_Feature* const* features) noexcept
{
    const auto* map_path = find<LV2_State_Map_Path>(features, LV2_STATE__mapPath);
    const auto* make_path = find<LV2_State_Make_Path>(features, LV2_STATE__makePath);
    const Paths paths{find<LV2_State_Free_Path>(features, LV2_STATE__freePath)};
    // Without a place for files (Ingen's block duplicate) nothing is saved;
    // the copy starts with empty loops.
    if (!store || !map_path || !make_path) { return LV2_STATE_SUCCESS; }
    const int32_t count = int32_t(loops.shared_count());
    store(handle, keys.loops, &count, sizeof(count), keys.int_type, kFlags);
    loops.begin_save();
    try {
        std::vector<float> audio;
        for (uint32_t n = 0; n < kMaxLoops; ++n) {
            const auto* loop = loops.shared(n);
            uint64_t cycle = 0;
            if (!loop || !loop->capture(audio, cycle)) { continue; }
            const auto name = "loop" + std::to_string(n + 1) + ".wav";
            char* path = make_path->path(make_path->handle, name.c_str());
            char* abstract = path && write_wav(path, audio, uint32_t(std::lround(rate)))
                ? map_path->abstract_path(map_path->handle, path) : nullptr;
            if (abstract) {
                const auto length = int64_t(cycle);
                const float seconds = loop->seconds();
                store(handle, keys.file[n], abstract, std::strlen(abstract) + 1, keys.path, kFlags);
                store(handle, keys.cycle[n], &length, sizeof(length), keys.long_type, kFlags);
                store(handle, keys.memory[n], &seconds, sizeof(seconds), keys.float_type, kFlags);
            }
            paths.release(abstract);
            paths.release(path);
        }
    } catch (...) {} // Out of memory: the loops copied so far are saved.
    loops.end_save();
    return LV2_STATE_SUCCESS;
}

LV2_State_Status restore_state(LoopSet& loops, const StateKeys& keys, LV2_State_Retrieve_Function retrieve,
                               LV2_State_Handle handle, const LV2_Feature* const* features) noexcept
{
    const auto* map_path = find<LV2_State_Map_Path>(features, LV2_STATE__mapPath);
    const Paths paths{find<LV2_State_Free_Path>(features, LV2_STATE__freePath)};
    size_t size;
    uint32_t type, flags;
    const auto* value = retrieve(handle, keys.loops, &size, &type, &flags);
    if (!value || type != keys.int_type || size != sizeof(int32_t)) { return LV2_STATE_SUCCESS; }
    int32_t count;
    std::memcpy(&count, value, sizeof(count));
    try {
        std::vector<float> left, right;
        for (uint32_t n = 0; n < kMaxLoops; ++n) {
            left.clear();
            right.clear();
            uint64_t cycle = 0;
            float seconds = 40;
            const auto* file = static_cast<const char*>(retrieve(handle, keys.file[n], &size, &type, &flags));
            if (file && type == keys.path && size && !file[size - 1]) {
                char* path = map_path ? map_path->absolute_path(map_path->handle, file) : nullptr;
                uint32_t file_rate = 0;
                // A missing or unreadable file leaves the loop empty. Another
                // sample rate is not converted: the loop plays at a new pitch.
                if (!read_wav(path ? path : file, left, right, file_rate)) { left.clear(); right.clear(); }
                paths.release(path);
                if (const auto* v = retrieve(handle, keys.cycle[n], &size, &type, &flags); v && type == keys.long_type && size == 8) {
                    int64_t c;
                    std::memcpy(&c, v, 8);
                    cycle = c > 0 ? uint64_t(c) : 0;
                }
                if (const auto* v = retrieve(handle, keys.memory[n], &size, &type, &flags); v && type == keys.float_type && size == 4) {
                    std::memcpy(&seconds, v, 4);
                    if (!std::isfinite(seconds)) { seconds = 40; }
                    seconds = std::clamp(seconds, 1.0f, float(kMaxSeconds));
                }
            }
            loops.restore(n, left.data(), right.data(), left.size(), cycle, seconds);
        }
    } catch (...) {} // Out of memory reading a file: the rest stay as they are.
    loops.hold(uint32_t(std::clamp(count, 1, int32_t(kMaxLoops))));
    return LV2_STATE_SUCCESS;
}
} // namespace loopi
