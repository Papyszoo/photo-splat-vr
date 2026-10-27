#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "common/path.h"
#include "common/status.h"

namespace splat {

// SHA-256 (FIPS 180-4). The checkpoint is 2.6 GB and spec rule 2 requires the
// pinned hash be verified before conversion, so the hash has to stream: this
// class keeps one 64-byte block, never the input.
class Sha256 {
public:
    static constexpr size_t kDigestBytes = 32;

    Sha256();

    void update(const void* data, size_t size);
    // Finishes the hash. Only call once, and do not update() afterwards.
    std::array<uint8_t, kDigestBytes> finish();

    static std::array<uint8_t, kDigestBytes> of(const void* data, size_t size);
    // Lowercase hex, 64 characters.
    static std::string hex(const std::array<uint8_t, kDigestBytes>& digest);
    static std::string of_hex(const void* data, size_t size);

private:
    void compress(const uint8_t block[64]);

    std::array<uint32_t, 8> state_{};
    uint8_t block_[64] = {};
    size_t block_len_ = 0;
    uint64_t bit_count_ = 0;
};

// Streams the file in 1 MiB chunks. Returns the lowercase hex digest.
Result<std::string> sha256_file(const Path& path, size_t chunk_bytes = 1u << 20);

}  // namespace splat
