// The two tensor streamers the SHARP converter uses, shared by the file path
// (converter.cpp reads the checkpoint with ZipArchive) and the streaming path
// (stream_converter.cpp receives the checkpoint's bytes from the network).
//
// Both take one payload chunk at a time and only keep one small output buffer
// alive, so a 2.8 GB checkpoint never has to be in memory.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "common/half.h"
#include "common/status.h"
#include "model/gguf_writer.h"

namespace splat {

// Streams float32 elements into the GGUF writer as fp16. Handles chunk
// boundaries that are not a multiple of four bytes by carrying up to three
// bytes over.
class Fp16Streamer {
public:
    explicit Fp16Streamer(GgufWriter& writer) : writer_(writer) { out_.reserve(1u << 16); }

    Status begin() { return writer_.begin_tensor(&expected_bytes_); }

    bool operator()(uint64_t, const void* data, size_t size) {
        if (!status_.ok()) return false;
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        size_t remaining = size;

        if (carry_len_ > 0) {
            while (carry_len_ < sizeof(carry_) && remaining > 0) {
                carry_[carry_len_++] = *bytes++;
                --remaining;
            }
            if (carry_len_ < sizeof(carry_)) return true;
            emit_element(carry_);
            carry_len_ = 0;
            if (!status_.ok()) return false;
        }

        const size_t elements = remaining / sizeof(carry_);
        if (elements > 0) {
            out_.resize(elements);
            for (size_t i = 0; i < elements; ++i) {
                float value = 0.0f;
                std::memcpy(&value, bytes + i * sizeof(carry_), sizeof(value));
                out_[i] = float_to_half(value);
            }
            status_ = writer_.append_tensor_bytes(out_.data(), elements * sizeof(uint16_t));
            if (!status_.ok()) return false;
            produced_ += elements;
            bytes += elements * sizeof(carry_);
            remaining -= elements * sizeof(carry_);
        }

        if (remaining > 0) {
            std::memcpy(carry_ + carry_len_, bytes, remaining);
            carry_len_ += remaining;
        }
        return true;
    }

    Status finish() {
        if (!status_.ok()) return status_;
        if (carry_len_ != 0) return Status::error("tensor payload is not a whole number of float32 elements");
        if (produced_ * sizeof(uint16_t) != expected_bytes_) {
            return Status::error("tensor produced " + std::to_string(produced_ * sizeof(uint16_t)) +
                                 " of " + std::to_string(expected_bytes_) + " bytes");
        }
        return writer_.end_tensor();
    }

    uint64_t produced() const { return produced_; }

private:
    void emit_element(const uint8_t* bytes) {
        float value = 0.0f;
        std::memcpy(&value, bytes, sizeof(value));
        const uint16_t half = float_to_half(value);
        status_ = writer_.append_tensor_bytes(&half, sizeof(half));
        if (!status_.ok()) return;
        ++produced_;
    }

    GgufWriter& writer_;
    std::vector<uint16_t> out_;
    uint8_t carry_[4] = {};
    size_t carry_len_ = 0;
    uint64_t produced_ = 0;
    uint64_t expected_bytes_ = 0;
    Status status_;
};

// 1-D tensors are copied through untouched; they stay fp32.
class PassthroughStreamer {
public:
    explicit PassthroughStreamer(GgufWriter& writer) : writer_(writer) {}

    Status begin() { return writer_.begin_tensor(&expected_bytes_); }

    bool operator()(uint64_t, const void* data, size_t size) {
        if (!status_.ok()) return false;
        status_ = writer_.append_tensor_bytes(data, size);
        written_ += size;
        return status_.ok();
    }

    Status finish() {
        if (!status_.ok()) return status_;
        if (written_ != expected_bytes_) {
            return Status::error("tensor wrote " + std::to_string(written_) + " of " +
                                 std::to_string(expected_bytes_) + " bytes");
        }
        return writer_.end_tensor();
    }

    uint64_t written() const { return written_; }

private:
    GgufWriter& writer_;
    uint64_t written_ = 0;
    uint64_t expected_bytes_ = 0;
    Status status_;
};

}  // namespace splat
