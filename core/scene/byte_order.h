#pragma once

// Little-endian byte accessors shared by the .ply and .psplat readers and
// writers. Both formats are defined as little-endian, and both files used to
// carry their own copy of these four functions.

#include <cstdint>
#include <cstring>

namespace splat {

inline void store_u32(uint8_t* p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value & 0xffu);
    p[1] = static_cast<uint8_t>((value >> 8) & 0xffu);
    p[2] = static_cast<uint8_t>((value >> 16) & 0xffu);
    p[3] = static_cast<uint8_t>((value >> 24) & 0xffu);
}

inline uint32_t load_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline void store_f32(uint8_t* p, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    store_u32(p, bits);
}

inline float load_f32(const uint8_t* p) {
    const uint32_t bits = load_u32(p);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

}  // namespace splat
