#include "model/checkpoint.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace splat {
namespace {

constexpr uint64_t kMaxPickleBytes = 8ull << 20;  // the real data.pkl is 305 KB

std::string dirname_of(const std::string& entry) {
    const size_t slash = entry.find_last_of('/');
    if (slash == std::string::npos) return {};
    return entry.substr(0, slash + 1);
}

std::string join_shape(const std::vector<int64_t>& shape) {
    std::string text = "(";
    for (size_t i = 0; i < shape.size(); ++i) {
        if (i != 0) text += ", ";
        text += std::to_string(shape[i]);
    }
    text += ")";
    return text;
}

}  // namespace

Result<Checkpoint> open_checkpoint(const Path& path) {
    Checkpoint checkpoint;
    checkpoint.path = path;

    Status status = checkpoint.zip.open(path);
    if (!status.ok()) return Result<Checkpoint>::failure(status.message());
    checkpoint.zip64 = checkpoint.zip.is_zip64();

    // Locate "<prefix>/data.pkl": torch.save v1 zipfile layout has exactly one.
    const std::string* pickle_entry = nullptr;
    for (const ZipEntryInfo& entry : checkpoint.zip.entries()) {
        if (entry.name.size() >= 9 && entry.name.compare(entry.name.size() - 9, 9, "/data.pkl") == 0) {
            if (pickle_entry != nullptr) {
                return Result<Checkpoint>::failure(path.string() +
                                                   ": zip has more than one data.pkl entry");
            }
            pickle_entry = &entry.name;
        }
    }
    if (pickle_entry == nullptr) {
        return Result<Checkpoint>::failure(path.string() +
                                           ": no <prefix>/data.pkl entry; not a torch.save zipfile");
    }
    checkpoint.prefix = dirname_of(*pickle_entry);

    // torch writes the byte order next to the pickle; big-endian hosts are not
    // supported by the converter (spec targets little-endian consoles).
    const int order_index = checkpoint.zip.find(checkpoint.prefix + "byteorder");
    if (order_index >= 0) {
        const Result<std::vector<uint8_t>> order_bytes =
            checkpoint.zip.read_entry(checkpoint.prefix + "byteorder", 64);
        if (!order_bytes.ok()) return Result<Checkpoint>::failure(order_bytes.message());
        checkpoint.byteorder.assign(order_bytes.value().begin(), order_bytes.value().end());
        if (checkpoint.byteorder != "little") {
            return Result<Checkpoint>::failure(path.string() + ": checkpoint byteorder is '" +
                                               checkpoint.byteorder + "', only little-endian is read");
        }
    }

    const Result<std::vector<uint8_t>> pickle =
        checkpoint.zip.read_entry(*pickle_entry, kMaxPickleBytes);
    if (!pickle.ok()) return Result<Checkpoint>::failure(pickle.message());

    const Result<PickleState> state =
        unpickle_state_dict(pickle.value(), path.string() + ":" + *pickle_entry);
    if (!state.ok()) return Result<Checkpoint>::failure(state.message());

    checkpoint.tensors.reserve(state.value().tensors.size());
    for (const TensorRecord& record : state.value().tensors) {
        if (record.dtype != StorageDtype::Float32) {
            return Result<Checkpoint>::failure(path.string() + ": tensor '" + record.name +
                                               "' is not float32; only float32 checkpoints convert");
        }
        const uint64_t numel = record.numel();
        if (numel == TensorRecord::kInvalidNumel) {
            return Result<Checkpoint>::failure(path.string() + ": tensor '" + record.name +
                                               "' shape " + join_shape(record.shape) +
                                               " overflows a 64-bit element count");
        }
        if (numel == 0) {
            return Result<Checkpoint>::failure(path.string() + ": tensor '" + record.name +
                                               "' shape " + join_shape(record.shape) +
                                               " has no elements");
        }
        if (!record.contiguous()) {
            return Result<Checkpoint>::failure(
                path.string() + ": tensor '" + record.name + "' shape " + join_shape(record.shape) +
                " is not C-contiguous; strided views are not supported");
        }
        if (record.storage_offset < 0) {
            return Result<Checkpoint>::failure(path.string() + ": tensor '" + record.name +
                                               "' has a negative storage offset");
        }
        const uint64_t offset = static_cast<uint64_t>(record.storage_offset);
        if (offset > record.storage_numel || numel > record.storage_numel - offset) {
            return Result<Checkpoint>::failure(
                path.string() + ": tensor '" + record.name + "' needs elements up to " +
                std::to_string(offset + numel) + " but its storage holds " +
                std::to_string(record.storage_numel));
        }

        CheckpointTensor tensor;
        tensor.zip_name = checkpoint.prefix + "data/" + record.storage_key;
        const int entry_index = checkpoint.zip.find(tensor.zip_name);
        if (entry_index < 0) {
            return Result<Checkpoint>::failure(path.string() + ": zip entry '" + tensor.zip_name +
                                               "' for tensor '" + record.name + "' is missing");
        }
        // No storage can be larger than the file, so this bound also keeps the
        // byte-size multiplication below from wrapping.
        const uint64_t actual_bytes =
            checkpoint.zip.entries()[static_cast<size_t>(entry_index)].uncompressed_bytes;
        if (record.storage_numel > UINT64_MAX / 4) {
            return Result<Checkpoint>::failure(path.string() + ": tensor '" + record.name +
                                               "' storage is too large to be a float32 tensor");
        }
        const uint64_t expected_bytes = record.storage_numel * 4;
        if (actual_bytes != expected_bytes) {
            return Result<Checkpoint>::failure(
                path.string() + ": zip entry '" + tensor.zip_name + "' is " +
                std::to_string(actual_bytes) + " bytes, expected " +
                std::to_string(expected_bytes) + " for tensor '" + record.name + "'");
        }
        tensor.record = record;
        checkpoint.tensors.push_back(std::move(tensor));
    }

    return Result<Checkpoint>::success(std::move(checkpoint));
}

Status read_checkpoint_tensor(ZipArchive& zip, const CheckpointTensor& tensor, const ZipSink& sink) {
    const size_t itemsize = storage_itemsize(tensor.record.dtype);
    const uint64_t skip = static_cast<uint64_t>(tensor.record.storage_offset) * itemsize;
    const uint64_t want = tensor.record.numel() * itemsize;

    uint64_t skipped = 0;
    uint64_t emitted = 0;
    bool aborted = false;
    const Status status = zip.stream_entry(tensor.zip_name, [&](uint64_t, const void* data, size_t size) {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        size_t remaining = size;
        if (skipped < skip) {
            const uint64_t drop = std::min<uint64_t>(skip - skipped, remaining);
            skipped += drop;
            bytes += drop;
            remaining -= static_cast<size_t>(drop);
        }
        if (remaining == 0) return true;
        if (emitted + remaining > want) {
            aborted = true;
            return false;
        }
        if (!sink(emitted, bytes, remaining)) {
            aborted = true;
            return false;
        }
        emitted += remaining;
        return true;
    });
    if (!status.ok()) return status;
    if (aborted) return Status::error("tensor '" + tensor.record.name + "' produced more bytes than expected");
    if (emitted != want) {
        return Status::error("tensor '" + tensor.record.name + "' yielded " + std::to_string(emitted) +
                             " of " + std::to_string(want) + " bytes");
    }
    return Status::success();
}

}  // namespace splat
