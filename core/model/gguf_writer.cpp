#include "model/gguf_writer.h"

#include <cstdio>
#include <cstring>
#include <utility>

#include "ggml.h"
#include "gguf.h"

#include "model/io_hint.h"

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
    // Read+write: patch_string() rewrites one metadata value in place after the
    // stream's digest is known. A plain converter never reads the file.
    file_ = std::fopen(path.string().c_str(), "w+b");
    if (file_ == nullptr) return Status::error("cannot open " + path.string() + " for writing");
    // One write per MiB, plus the kGgufSyncBatchBytes fsync batches in write_raw().
    stream_buffer_.assign(1u << 20, 0);
    if (std::setvbuf(file_, stream_buffer_.data(), _IOFBF, stream_buffer_.size()) != 0) {
        std::fclose(file_);
        file_ = nullptr;
        return Status::error("cannot buffer " + path.string());
    }
    synced_bytes_ = 0;
    return Status::success();
}

Status GgufWriter::write_header() {
    if (file_ == nullptr) return Status::error("GGUF writer is not open");
    if (header_written_) return Status::error("GGUF header already written");
    if (!gguf_write_to_file_ptr(gguf_, file_, /*only_meta=*/true)) {
        return Status::error("cannot write the GGUF header");
    }
    // The stream buffer holds ~1 MiB; flush before measuring so the data offset
    // is a real file position in every libc's ftell.
    if (std::fflush(file_) != 0) return Status::error("cannot flush the GGUF header");
    const long offset = std::ftell(file_);
    if (offset < 0) return Status::error("cannot determine the GGUF data offset");
    data_offset_ = static_cast<uint64_t>(offset);
    bytes_written_ = data_offset_;
    header_written_ = true;
    return Status::success();
}

namespace {

// The value sizes of the GGUF scalar types patch_string() has to walk over
// (third_party/ggml/include/gguf.h). Strings and arrays are handled in code.
uint64_t gguf_scalar_size(uint32_t type) {
    switch (type) {
        case 0:  // UINT8
        case 1:  // INT8
        case 7:  // BOOL
            return 1;
        case 2:  // UINT16
        case 3:  // INT16
            return 2;
        case 4:  // UINT32
        case 5:  // INT32
        case 6:  // FLOAT32
            return 4;
        case 10:  // UINT64
        case 11:  // INT64
        case 12:  // FLOAT64
            return 8;
        default:
            return 0;  // STRING/ARRAY: not a plain scalar
    }
}

uint64_t read_le(const std::vector<uint8_t>& bytes, size_t offset, size_t width) {
    uint64_t value = 0;
    for (size_t i = 0; i < width; ++i) value |= static_cast<uint64_t>(bytes[offset + i]) << (8 * i);
    return value;
}

}  // namespace

Status GgufWriter::patch_string(const std::string& key, const std::string& value) {
    if (file_ == nullptr) return Status::error("GGUF writer is not open");
    if (!header_written_) return Status::error("the GGUF header has not been written yet");

    // The header (metadata + tensor table + padding) is small (157 KB for
    // SHARP), so it can be read back to find the exact value offset.
    if (std::fflush(file_) != 0) return Status::error("cannot flush the GGUF header");
    std::vector<uint8_t> header(static_cast<size_t>(data_offset_));
    if (std::fseek(file_, 0, SEEK_SET) != 0 ||
        std::fread(header.data(), 1, header.size(), file_) != header.size()) {
        return Status::error("cannot read the GGUF header back");
    }

    const auto restore_end = [this]() -> bool {
        return std::fseek(file_, 0, SEEK_END) == 0 &&
               std::ftell(file_) == static_cast<long>(bytes_written_);
    };

    if (header.size() < 24 || std::memcmp(header.data(), "GGUF", 4) != 0) {
        restore_end();
        return Status::error("the GGUF header has no GGUF magic");
    }
    size_t at = 8;  // magic (4) + version (4)
    const uint64_t kv_count = read_le(header, at, 8);
    at += 16;  // tensor count + kv count

    for (uint64_t index = 0; index < kv_count; ++index) {
        if (at + 8 > header.size()) break;
        const uint64_t key_bytes = read_le(header, at, 8);
        at += 8;
        if (key_bytes > header.size() - at) break;
        const std::string entry_key(reinterpret_cast<const char*>(header.data() + at),
                                    static_cast<size_t>(key_bytes));
        at += static_cast<size_t>(key_bytes);
        if (at + 4 > header.size()) break;
        const uint32_t type = static_cast<uint32_t>(read_le(header, at, 4));
        at += 4;

        if (type == 8) {  // GGUF_TYPE_STRING
            if (at + 8 > header.size()) break;
            const uint64_t length = read_le(header, at, 8);
            at += 8;
            if (length > header.size() - at) break;
            if (entry_key == key) {
                if (length != value.size()) {
                    restore_end();
                    return Status::error("metadata '" + key + "' is " + std::to_string(length) +
                                         " bytes, the replacement is " +
                                         std::to_string(value.size()) +
                                         "; only an in-place replacement is supported");
                }
                if (std::fseek(file_, static_cast<long>(at), SEEK_SET) != 0 ||
                    std::fwrite(value.data(), 1, value.size(), file_) != value.size() ||
                    std::fflush(file_) != 0) {
                    restore_end();
                    return Status::error("cannot patch the metadata '" + key + "'");
                }
                // The digest is written after the data batches were synced, so
                // it must be synced too: a power cut after Ready would otherwise
                // leave the 64-'0' placeholder and a later verify_gguf would
                // refuse the file.
                if (!splat::sync_and_drop(::fileno(file_), 0, data_offset_)) {
                    restore_end();
                    return Status::error("cannot sync the patched GGUF header to the disk");
                }
                synced_bytes_ = bytes_written_;
                if (!restore_end()) return Status::error("cannot return to the GGUF's data section");
                return Status::success();
            }
            at += static_cast<size_t>(length);
            continue;
        }
        if (type == 9) {  // GGUF_TYPE_ARRAY
            if (at + 12 > header.size()) break;
            const uint32_t element_type = static_cast<uint32_t>(read_le(header, at, 4));
            const uint64_t count = read_le(header, at + 4, 8);
            at += 12;
            if (element_type == 8) {
                for (uint64_t i = 0; i < count; ++i) {
                    if (at + 8 > header.size()) {
                        at = header.size() + 1;
                        break;
                    }
                    const uint64_t length = read_le(header, at, 8);
                    at += 8;
                    if (length > header.size() - at) {
                        at = header.size() + 1;
                        break;
                    }
                    at += static_cast<size_t>(length);
                }
                if (at > header.size()) break;
                continue;
            }
            const uint64_t element_size = gguf_scalar_size(element_type);
            if (element_size == 0 || count > (header.size() - at) / element_size) break;
            at += static_cast<size_t>(count * element_size);
            continue;
        }
        const uint64_t size = gguf_scalar_size(type);
        if (size == 0 || size > header.size() - at) break;
        at += static_cast<size_t>(size);
    }

    restore_end();
    return Status::error("the GGUF header has no string metadata '" + key + "'");
}

Status GgufWriter::write_raw(const void* data, size_t size) {
    if (size == 0) return Status::success();
    if (std::fwrite(data, 1, size, file_) != size) return Status::error("short write to the GGUF file");
    bytes_written_ += size;
    if (bytes_written_ - synced_bytes_ >= kGgufSyncBatchBytes) {
        // Bound the dirty pages of a 1.3 GB output: sync each batch and drop it
        // from the page cache (io_hint.h). A failed flush or fsync is a failed
        // conversion, like a short write: the bytes are not on the disk.
        if (std::fflush(file_) != 0) return Status::error("cannot flush the GGUF file");
        if (!splat::sync_and_drop(::fileno(file_), synced_bytes_, bytes_written_ - synced_bytes_)) {
            return Status::error("cannot sync the GGUF file to the disk");
        }
        synced_bytes_ = bytes_written_;
    }
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
    bool synced = true;
    if (flushed && bytes_written_ > synced_bytes_) {
        // The bytes written since the last batch: put them on the disk and drop
        // them as well. A failed fsync fails the conversion, as a failed write
        // does. When patch_string() already synced the file this is a no-op.
        synced =
            splat::sync_and_drop(::fileno(file_), synced_bytes_, bytes_written_ - synced_bytes_);
        synced_bytes_ = bytes_written_;
    }
    close();
    if (!flushed) return Status::error("cannot flush the GGUF file");
    if (!synced) return Status::error("cannot sync the GGUF file to the disk");
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
    synced_bytes_ = 0;
}

size_t GgufWriter::tensor_count() const { return tensors_.size(); }

const std::vector<GgufTensorDesc>& GgufWriter::tensors() const { return tensors_; }

uint64_t GgufWriter::data_offset() const { return data_offset_; }

uint64_t GgufWriter::bytes_written() const { return bytes_written_; }

}  // namespace splat
