#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "common/path.h"
#include "common/status.h"

struct ggml_context;
struct gguf_context;

namespace splat {

// The writer syncs (and drops) every this many bytes. Smaller than the
// download's 64 MiB batch (model/io_hint.h) on purpose: in the streaming path
// the writer's sync runs inside the network fetcher's sink call, where the
// stall watchdog sees no byte until it returns, and the console's /data writes
// run at 1-2 MB/s. An 8 MiB sync is ~4-8 s there, far below the 60 s deadline,
// so bounding one batch's dirty pages also bounds one invisible stall.
inline constexpr uint64_t kGgufSyncBatchBytes = 8ull << 20;

struct GgufTensorDesc {
    std::string name;             // GGUF tensor name, at most 63 bytes (ggml limit)
    std::vector<int64_t> shape;   // torch order, outermost dimension first
    bool fp16 = true;             // false keeps float32
};

// Streaming GGUF writer built on ggml's gguf API.
//
// gguf_write_to_file() wants every tensor's bytes in memory at once, which would
// cost 2.8 GB for the SHARP checkpoint. Instead the header, metadata and tensor
// info block is written with gguf_write_to_file_ptr(ctx, file, only_meta=true),
// and the payloads are appended one tensor at a time through
// write_tensor_bytes(). Tensor offsets inside the data section come from
// gguf_get_tensor_offset(), so the file is a regular GGUF that gguf_init_from_file
// reads back.
class GgufWriter {
public:
    GgufWriter();
    ~GgufWriter();
    GgufWriter(const GgufWriter&) = delete;
    GgufWriter& operator=(const GgufWriter&) = delete;

    // Creates the output file and registers the tensor list. Tensor names must
    // be unique and at most 63 bytes; shapes must have 1 to 4 dimensions.
    Status begin(const Path& path, std::vector<GgufTensorDesc> tensors);

    // Metadata. Call after begin() and before write_header().
    void set_string(const std::string& key, const std::string& value);
    void set_u32(const std::string& key, uint32_t value);
    void set_u64(const std::string& key, uint64_t value);
    void set_strings(const std::string& key, const std::vector<std::string>& values);

    // Replaces the value of a string metadata entry that is already on disk,
    // in place. The streaming converter needs this for the checkpoint digest:
    // the header must be written before the first tensor arrives, but the
    // digest is only known after the last byte. The new value must have the
    // same length as the old one (a SHA-256 stays 64 hex characters). Only
    // valid after write_header() and before finish(); the data section and
    // bytes_written() are not moved.
    Status patch_string(const std::string& key, const std::string& value);

    // Writes header, metadata, tensor info and the alignment padding that
    // precedes the data section.
    Status write_header();

    // Payloads are appended one tensor at a time, in declaration order.
    // begin_tensor() reports the exact byte size the tensor must produce;
    // append_tensor_bytes() may be called any number of times in between.
    Status begin_tensor(uint64_t* bytes);
    Status append_tensor_bytes(const void* data, size_t size);
    Status end_tensor();

    // Whole-tensor convenience wrapper: begin + append + end.
    Status write_tensor_bytes(const void* data, size_t size);

    // Pads to the alignment, flushes and closes. Returns an error if any
    // declared tensor is still missing.
    Status finish();

    size_t tensor_count() const;
    const std::vector<GgufTensorDesc>& tensors() const;
    uint64_t data_offset() const;   // file offset where the data section starts
    uint64_t bytes_written() const; // bytes on disk so far

private:
    Status open_file(const Path& path);
    void close();
    uint64_t current_offset() const;
    Status write_raw(const void* data, size_t size);

    ggml_context* ggml_ = nullptr;
    gguf_context* gguf_ = nullptr;
    std::FILE* file_ = nullptr;
    // The stream's own buffer: one 1 MiB write per flush instead of one per
    // 32 KiB converter chunk (the console's write cost is per call,
    // runs/issue-1-crash/sharp-slow-report.md §1.4/§2.6).
    std::vector<char> stream_buffer_;
    uint64_t synced_bytes_ = 0;  // bytes up to this offset are fsynced
    bool header_written_ = false;
    size_t next_tensor_ = 0;
    uint64_t tensor_bytes_ = 0;  // bytes appended for the current tensor
    bool tensor_open_ = false;
    uint64_t bytes_written_ = 0;
    uint64_t data_offset_ = 0;
    std::vector<GgufTensorDesc> tensors_;
};

}  // namespace splat
