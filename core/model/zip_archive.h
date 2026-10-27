#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common/path.h"
#include "common/status.h"

namespace splat {

struct ZipEntryInfo {
    std::string name;
    uint64_t uncompressed_bytes = 0;
    uint64_t compressed_bytes = 0;
    uint32_t method = 0;  // 0 = stored, 8 = deflate
};

// Streaming receiver for one entry: called with successive byte ranges whose
// offsets concatenate to 0, n1, n1 + n2, ... . Returning false aborts the read.
using ZipSink = std::function<bool(uint64_t offset, const void* data, size_t size)>;

// Read-only zip archive over the vendored miniz (third_party/miniz). A torch
// checkpoint is a zip, and it is 2.6 GB, so entries are always streamed in
// fixed-size chunks instead of being decompressed into memory.
class ZipArchive {
public:
    ZipArchive();
    ~ZipArchive();
    ZipArchive(const ZipArchive&) = delete;
    ZipArchive& operator=(const ZipArchive&) = delete;
    ZipArchive(ZipArchive&& other) noexcept;
    ZipArchive& operator=(ZipArchive&& other) noexcept;

    Status open(const Path& path);
    void close();
    bool is_open() const;

    // True when the archive carries zip64 markers. mz_zip handles both forms;
    // the flag is surfaced so callers and the CLI can report which one was read.
    bool is_zip64() const;

    const std::vector<ZipEntryInfo>& entries() const;
    // Index of an entry by exact name, or -1.
    int find(const std::string& name) const;

    // Reads a whole (small) entry into memory. max_bytes guards against reading
    // a tensor blob by accident.
    Result<std::vector<uint8_t>> read_entry(const std::string& name, uint64_t max_bytes);

    // Streams one entry through `sink`. Chunks are bounded by miniz's own read
    // buffer (64 KiB), never by the entry size.
    Status stream_entry(const std::string& name, const ZipSink& sink);
    Status stream_entry(const ZipEntryInfo& entry, const ZipSink& sink);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace splat
