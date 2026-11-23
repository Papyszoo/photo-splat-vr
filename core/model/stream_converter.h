// Convert a torch.save checkpoint while it is still being downloaded.
//
// The console's measured bottleneck is its /data write path: writing the
// 2.81 GB .pt and then converting it costs two writes of the checkpoint's data
// (and a re-read), and the write rate collapses once ~1.5 GB is dirty
// (runs/issue-1-crash/sharp-slow-report.md). This converter consumes the
// network body directly and writes only the 1.31 GB GGUF: the .pt is never
// stored.
//
// The zip layout makes it possible: data.pkl is the first entry and holds every
// tensor's dtype, shape and storage size, every entry is stored (method 0), and
// a fixed 16-byte data descriptor follows each entry with its CRC and sizes.
// The entry order the pinned checkpoint uses is "storage keys sorted as
// strings" (torch.save's order for this file); the header therefore can be
// written before the first tensor arrives. A file with a different order fails
// on the first mismatching entry and never produces a wrong GGUF.
//
// The whole stream is hashed on the way through. The header is written with a
// 64-character placeholder for the digest; finish() patches it in place once
// the last byte has been seen, so the output is byte-identical to the file
// converter's.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "common/path.h"
#include "common/status.h"
#include "model/converter.h"

namespace splat {

struct StreamConvertOptions {
    std::string backend = "sharp";
    std::string source_filename;  // goes into sharp.source_filename
    uint64_t source_bytes = 0;    // pinned size; finish() rejects a different total
    bool keep_fp32 = false;       // the converter's --fp32 rule
    // One line every ~5 s of tensor I/O and one total line. Empty: no lines.
    std::function<void(const std::string&)> log;
};

class StreamConverter {
public:
    StreamConverter(Path output, StreamConvertOptions options);
    ~StreamConverter();
    StreamConverter(const StreamConverter&) = delete;
    StreamConverter& operator=(const StreamConverter&) = delete;

    // Handles the next bytes of the checkpoint, in order. False/error means the
    // stream is not a convertible checkpoint (or the output write failed); the
    // partial GGUF is removed by abort()/the destructor.
    Status feed(const void* data, size_t size);

    // All bytes have been fed: checks the entry plan, the total size and the
    // archive's end record, patches the SHA-256 into the GGUF and closes it. The
    // digest is in stats.source_sha256; the caller compares it with the pin and
    // deletes the file on a mismatch.
    Result<ConvertStats> finish();

    // Stops the conversion and removes the partial output. Safe to call twice.
    void abort();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace splat
