#include "model/gguf_writer.h"

#include <cstring>
#include <utility>

#include "ggml.h"
#include "gguf.h"

namespace splat {
namespace {

// ggml stores tensor structs in a context arena. The converter registers ~900
// tensors of metadata and no tensor data, so a few MiB is plenty.
constexpr size_t kTensorArenaBytes = 16u << 20;

}  // namespace

GgufWriter::GgufWriter() = default;

GgufWriter::~GgufWriter() { close(); }

Status GgufWriter::begin(const Path& path, std::vector<GgufTensorDesc> tensors) {
    close();
    tensors_ = std::move(tensors);
    if (tensors_.empty()) return Status::error("GGUF writer needs at least one tensor");

    ggml_init_params params{};
    params.mem_size = kTensorArenaBytes;
    params.mem_buffer = nullptr;
    params.no_alloc = true;
    ggml_ = ggml_init(params);
    if (ggml_ == nullptr) return Status::error("cannot create the ggml tensor context");

    gguf_ = gguf_init_empty();
    if (gguf_ == nullptr) {
        close();
        return Status::error("cannot create the gguf context");
    }

    for (const GgufTensorDesc& desc : tensors_) {
        if (desc.name.empty()) {
            close();
            return Status::error("tensor with an empty name");
        }
        if (desc.name.size() >= 64) {
            close();
            return Status::error("tensor name '" + desc.name + "' is " +
                                 std::to_string(desc.name.size()) +
                                 " bytes; ggml's limit is 63");
        }
        if (desc.shape.empty() || desc.shape.size() > 4) {
            close();
            return Status::error("tensor '" + desc.name + "' has rank " +
                                 std::to_string(desc.shape.size()) + "; only ranks 1..4 are written");
        }
        for (int64_t dim : desc.shape) {
            if (dim <= 0) {
                close();
                return Status::error("tensor '" + desc.name + "' has a non-positive dimension");
            }
        }

        // Torch shapes are outermost-first; GGUF/ggml store ne with ne[0] as the
        // fastest-moving dimension, so the shape is reversed. The bytes are not
        // touched: both are row-major over the same linear order.
        int64_t ne[4] = {1, 1, 1, 1};
        for (size_t i = 0; i < desc.shape.size(); ++i) {
            ne[desc.shape.size() - 1 - i] = desc.shape[i];
        }

        ggml_tensor* tensor = ggml_new_tensor(ggml_, desc.fp16 ? GGML_TYPE_F16 : GGML_TYPE_F32,
                                             static_cast<int>(desc.shape.size()), ne);
        if (tensor == nullptr) {
            close();
            return Status::error("cannot declare ggml tensor '" + desc.name + "'");
        }
        ggml_set_name(tensor, desc.name.c_str());
        if (gguf_find_tensor(gguf_, desc.name.c_str()) >= 0) {
            close();
            return Status::error("duplicate tensor name '" + desc.name + "'");
        }
        gguf_add_tensor(gguf_, tensor);
    }

    return open_file(path);
}

void GgufWriter::set_string(const std::string& key, const std::string& value) {
    gguf_set_val_str(gguf_, key.c_str(), value.c_str());
}

void GgufWriter::set_u32(const std::string& key, uint32_t value) {
    gguf_set_val_u32(gguf_, key.c_str(), value);
}

void GgufWriter::set_u64(const std::string& key, uint64_t value) {
    gguf_set_val_u64(gguf_, key.c_str(), value);
}

void GgufWriter::set_strings(const std::string& key, const std::vector<std::string>& values) {
    std::vector<const char*> pointers;
    pointers.reserve(values.size());
    for (const std::string& value : values) pointers.push_back(value.c_str());
    gguf_set_arr_str(gguf_, key.c_str(), pointers.data(), pointers.size());
}

Status GgufWriter::open_file(const Path& path) {
    file_ = std::fopen(path.string().c_str(), "wb");
    if (file_ == nullptr) return Status::error("cannot open " + path.string() + " for writing");
    return Status::success();
}

Status GgufWriter::write_header() {
    if (file_ == nullptr) return Status::error("GGUF writer is not open");
    if (header_written_) return Status::error("GGUF header already written");
    if (!gguf_write_to_file_ptr(gguf_, file_, /*only_meta=*/true)) {
        return Status::error("cannot write the GGUF header");
    }
    const long offset = std::ftell(file_);
    if (offset < 0) return Status::error("cannot determine the GGUF data offset");
    data_offset_ = static_cast<uint64_t>(offset);
    bytes_written_ = data_offset_;
    header_written_ = true;
    return Status::success();
}

Status GgufWriter::write_raw(const void* data, size_t size) {
    if (size == 0) return Status::success();
    if (std::fwrite(data, 1, size, file_) != size) return Status::error("short write to the GGUF file");
    bytes_written_ += size;
    return Status::success();
}

Status GgufWriter::begin_tensor(uint64_t* bytes) {
    if (!header_written_) return Status::error("GGUF header has not been written yet");
    if (tensor_open_) return Status::error("tensor '" + tensors_[next_tensor_].name + "' is still open");
    if (next_tensor_ >= tensors_.size()) return Status::error("more tensor data than tensors declared");

    const int64_t index = static_cast<int64_t>(next_tensor_);
    const uint64_t want_at = data_offset_ + gguf_get_tensor_offset(gguf_, index);
    if (bytes_written_ != want_at) {
        return Status::error("tensor '" + tensors_[next_tensor_].name + "' would start at offset " +
                             std::to_string(bytes_written_) + ", the header says " +
                             std::to_string(want_at));
    }
    tensor_bytes_ = 0;
    tensor_open_ = true;
    if (bytes != nullptr) *bytes = gguf_get_tensor_size(gguf_, index);
    return Status::success();
}

Status GgufWriter::append_tensor_bytes(const void* data, size_t size) {
    if (!tensor_open_) return Status::error("append_tensor_bytes() without begin_tensor()");
    const uint64_t expected = gguf_get_tensor_size(gguf_, static_cast<int64_t>(next_tensor_));
    if (tensor_bytes_ + size > expected) {
        return Status::error("tensor '" + tensors_[next_tensor_].name + "' would exceed its " +
                             std::to_string(expected) + " byte payload");
    }
    const Status status = write_raw(data, size);
    if (!status.ok()) return status;
    tensor_bytes_ += size;
    return Status::success();
}

Status GgufWriter::end_tensor() {
    if (!tensor_open_) return Status::error("end_tensor() without begin_tensor()");
    const uint64_t expected = gguf_get_tensor_size(gguf_, static_cast<int64_t>(next_tensor_));
    if (tensor_bytes_ != expected) {
        return Status::error("tensor '" + tensors_[next_tensor_].name + "' produced " +
                             std::to_string(tensor_bytes_) + " of " + std::to_string(expected) +
                             " bytes");
    }
    tensor_open_ = false;

    // gguf_write_out pads after every tensor; mirror that so the declared
    // offsets stay valid.
    const uint64_t alignment = gguf_get_alignment(gguf_);
    const uint64_t padded = (expected + alignment - 1) / alignment * alignment;
    static const uint8_t kZero[64] = {};
    size_t pad = static_cast<size_t>(padded - expected);
    while (pad > 0) {
        const size_t chunk = pad < sizeof(kZero) ? pad : sizeof(kZero);
        const Status status = write_raw(kZero, chunk);
        if (!status.ok()) return status;
        pad -= chunk;
    }

    ++next_tensor_;
    return Status::success();
}

Status GgufWriter::write_tensor_bytes(const void* data, size_t size) {
    Status status = begin_tensor(nullptr);
    if (!status.ok()) return status;
    status = append_tensor_bytes(data, size);
    if (!status.ok()) return status;
    return end_tensor();
}

Status GgufWriter::finish() {
    if (file_ == nullptr) return Status::error("GGUF writer is not open");
    if (tensor_open_) return Status::error("tensor '" + tensors_[next_tensor_].name + "' is still open");
    if (next_tensor_ != tensors_.size()) {
        return Status::error("GGUF file is missing " + std::to_string(tensors_.size() - next_tensor_) +
                             " tensors");
    }
    const bool flushed = std::fflush(file_) == 0;
    close();
    if (!flushed) return Status::error("cannot flush the GGUF file");
    return Status::success();
}

void GgufWriter::close() {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    if (gguf_ != nullptr) {
        gguf_free(gguf_);
        gguf_ = nullptr;
    }
    if (ggml_ != nullptr) {
        ggml_free(ggml_);
        ggml_ = nullptr;
    }
    header_written_ = false;
    tensor_open_ = false;
    tensor_bytes_ = 0;
    next_tensor_ = 0;
}

size_t GgufWriter::tensor_count() const { return tensors_.size(); }

const std::vector<GgufTensorDesc>& GgufWriter::tensors() const { return tensors_; }

uint64_t GgufWriter::data_offset() const { return data_offset_; }

uint64_t GgufWriter::bytes_written() const { return bytes_written_; }

}  // namespace splat
