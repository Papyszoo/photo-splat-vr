#include "model/stream_converter.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" {
#include "miniz/miniz.h"
}

#include "model/file_util.h"
#include "model/gguf_writer.h"
#include "model/sha256.h"
#include "model/tensor_stream.h"
#include "model/text_format.h"
#include "model/unpickler.h"
#include "model/zip_header.h"

namespace splat {
namespace {

constexpr uint8_t kDescriptorSignature[4] = {'P', 'K', 0x07, 0x08};
constexpr size_t kDescriptorBytes = 16;
// Same cap as the file path (checkpoint.cpp): the real data.pkl is 305 KB.
constexpr size_t kMaxPickleBytes = 8u << 20;
// torch metadata entries (byteorder, .format_version, .storage_alignment) are a
// handful of bytes. The end of one whose length the pickle does not give is
// found with its data descriptor; a longer scan means this is not the layout we
// can read in one pass.
constexpr size_t kMaxMetadataBytes = 4096;
// The pinned checkpoint's digest is 64 lowercase hex characters; the header is
// written with that many '0's and patched in place.
constexpr size_t kSha256HexBytes = 64;
// The zip end record ("PK\x05\x06", 18 more bytes, then the comment) closes the
// central directory. The checker keeps this much of the stream's tail: enough
// for the record plus a comment; a longer one fails cleanly.
constexpr size_t kTailWindowBytes = 4096;
constexpr std::chrono::milliseconds kLogInterval{5000};

}  // namespace

struct StreamConverter::Impl {
    enum class State { local_header, pickle, metadata, tensor, descriptor, tail, failed, done };

    // One storage entry of the checkpoint, in data.pkl order.
    struct Record {
        TensorRecord tensor;
        bool kept = false;
        uint64_t entry_bytes = 0;  // the whole data/<key> payload
    };

    Impl(Path output, StreamConvertOptions settings)
        : out(std::move(output)), options(std::move(settings)) {}

    Path out;
    StreamConvertOptions options;
    State state = State::local_header;
    std::string failure;

    // The unparsed tail of the fed bytes. A feed normally consumes its whole
    // chunk; the pickle and the small headers are the parts that wait.
    std::vector<uint8_t> bytes;
    size_t at = 0;
    // The last bytes of the archive (the central directory's end), kept so
    // finish() can require the zip end record instead of dropping the tail.
    std::vector<uint8_t> tail_bytes;

    std::vector<uint8_t> pickle;
    size_t next_pickle_attempt = 1024;  // exponential retry: see on_pickle_data()
    std::string prefix;  // "<folder>/" from the data.pkl entry

    std::vector<Record> records;  // data.pkl order
    std::unordered_map<std::string, size_t> by_key;
    std::vector<std::string> plan;  // storage keys in the zip's order
    size_t next_plan = 0;
    std::vector<std::string> dropped_names;
    std::vector<std::string> full_names;

    // The current entry.
    std::string name;
    bool has_descriptor = false;
    uint64_t entry_bytes = 0;  // declared payload size (0 when the descriptor has it)
    uint64_t entry_done = 0;
    uint64_t entry_skip = 0;  // storage offset in bytes
    uint64_t entry_want = 0;  // this tensor's bytes
    mz_ulong entry_crc = MZ_CRC32_INIT;
    bool entry_kept = false;
    std::string current_name;  // the tensor's torch name, for errors
    std::unique_ptr<Fp16Streamer> fp16;
    std::unique_ptr<PassthroughStreamer> fp32;

    // The descriptor that must follow the current entry.
    uint64_t descriptor_bytes = 0;
    mz_ulong descriptor_crc = MZ_CRC32_INIT;
    bool last_tensor_done = false;

    GgufWriter writer;
    uint64_t data_offset = 0;
    uint64_t kept_count = 0;
    uint64_t dropped_count = 0;
    uint64_t tensor_bytes_in = 0;
    uint64_t tensors_done = 0;

    Sha256 hasher;
    uint64_t total_in = 0;
    std::string digest;

    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_log = started;

    size_t available() const { return bytes.size() - at; }
    const uint8_t* cursor() const { return bytes.data() + at; }

    void consume(size_t count) {
        at += count;
        if (at == bytes.size()) {
            bytes.clear();
            at = 0;
        } else if (at >= (1u << 20)) {
            bytes.erase(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(at));
            at = 0;
        }
    }

    Status fail(const std::string& message) {
        if (failure.empty()) failure = message;
        state = State::failed;
        return Status::error(failure);
    }

    void log_line(const std::string& text) {
        if (options.log) options.log(text);
    }

    // One line every ~5 s of the stream: the console run needs a rate while the
    // body is arriving, not only when a tensor happens to finish.
    void maybe_log() {
        if (!options.log) return;
        const auto now = std::chrono::steady_clock::now();
        if (now - last_log < kLogInterval) return;
        const double elapsed = std::chrono::duration<double>(now - started).count();
        const uint64_t written = writer.bytes_written() > data_offset
                                     ? writer.bytes_written() - data_offset
                                     : 0;
        const std::string progress =
            plan.empty() ? std::string("headers") : (std::to_string(tensors_done) + "/" +
                                                     std::to_string(plan.size()) + " storages");
        log_line("stream: " + progress + ", " + gigabyte_text(total_in) + " in, " +
                 gigabyte_text(written) + " written, " + seconds_text(elapsed) + " s (" +
                 rate_text(total_in, elapsed) + " in, " + rate_text(written, elapsed) + " out)");
        last_log = now;
    }

    // ---- data.pkl -> tensor plan and GGUF header ---------------------------
    Status plan_and_begin(const PickleState& state_dict) {
        records.reserve(state_dict.tensors.size());
        for (const TensorRecord& record : state_dict.tensors) {
            if (record.dtype != StorageDtype::Float32) {
                return fail("tensor '" + record.name + "' is not float32; only float32 checkpoints convert");
            }
            const uint64_t numel = record.numel();
            if (numel == TensorRecord::kInvalidNumel) {
                return fail("tensor '" + record.name + "' has a shape that overflows a 64-bit element count");
            }
            if (numel == 0) return fail("tensor '" + record.name + "' has no elements");
            if (!record.contiguous()) {
                return fail("tensor '" + record.name + "' is not C-contiguous; strided views are not supported");
            }
            if (record.storage_offset < 0) {
                return fail("tensor '" + record.name + "' has a negative storage offset");
            }
            const uint64_t offset = static_cast<uint64_t>(record.storage_offset);
            if (offset > record.storage_numel || numel > record.storage_numel - offset) {
                return fail("tensor '" + record.name + "' needs elements up to " +
                            std::to_string(offset + numel) + " but its storage holds " +
                            std::to_string(record.storage_numel));
            }
            if (record.storage_numel > UINT64_MAX / 4) {
                return fail("tensor '" + record.name + "' storage is too large to be float32");
            }
            if (by_key.find(record.storage_key) != by_key.end()) {
                // One data/<key> entry can feed several tensor records in
                // torch.save; the streaming path would have to split one
                // forward pass between them. SHARP never does this.
                return fail("storage " + record.storage_key + " is used by more than one tensor ('" +
                            record.name +
                            "'); the streaming converter needs one tensor per storage");
            }
            Record entry;
            entry.tensor = record;
            entry.entry_bytes = record.storage_numel * 4;
            const size_t index = records.size();
            by_key.emplace(record.storage_key, index);
            if (is_dead_tensor(record.name)) {
                dropped_names.push_back(record.name);
            } else {
                entry.kept = true;
                tensor_bytes_in += numel * 4;
            }
            records.push_back(std::move(entry));
        }
        if (records.empty()) return fail("the checkpoint holds no tensor records");

        // The order of the data/<key> entries: the pinned checkpoint writes them
        // sorted as strings (data/0, data/1, data/10, ...), the one order the
        // pickle alone can predict. A file that writes them in another order
        // fails on the first mismatching entry instead of producing a GGUF.
        plan.reserve(records.size());
        for (const Record& record : records) plan.push_back(record.tensor.storage_key);
        std::sort(plan.begin(), plan.end());

        std::vector<GgufTensorDesc> descriptions;
        descriptions.reserve(records.size());
        full_names.reserve(records.size());
        for (const std::string& key : plan) {
            const Record& record = records[by_key.at(key)];
            if (!record.kept) continue;
            if (record.tensor.shape.size() > 4) {
                return fail("tensor '" + record.tensor.name + "' has rank " +
                            std::to_string(record.tensor.shape.size()) + "; GGUF converts ranks 1..4");
            }
            GgufTensorDesc description;
            description.name = gguf_tensor_name(record.tensor.name);
            description.shape = record.tensor.shape;
            description.fp16 = !options.keep_fp32 && record.tensor.shape.size() >= 2;
            descriptions.push_back(std::move(description));
            full_names.push_back(record.tensor.name);
        }
        kept_count = descriptions.size();
        dropped_count = dropped_names.size();
        if (descriptions.empty()) return fail("checkpoint has no convertible tensors");

        Status status = writer.begin(out, std::move(descriptions));
        if (!status.ok()) return fail(status.message());
        // A placeholder of exactly the digest's length: finish() replaces it
        // with the real SHA-256 once the last byte has been seen.
        write_sharp_gguf_metadata(writer, options.backend, std::string(kSha256HexBytes, '0'),
                                  options.source_filename, options.source_bytes,
                                  static_cast<uint32_t>(records.size()),
                                  static_cast<uint32_t>(kept_count),
                                  static_cast<uint32_t>(dropped_count), dropped_names, full_names,
                                  options.keep_fp32);
        status = writer.write_header();
        if (!status.ok()) return fail(status.message());
        data_offset = writer.data_offset();
        return Status::success();
    }

    Status on_pickle() {
        size_t consumed = 0;
        Result<PickleState> state_dict = unpickle_state_dict(pickle, out.string() + ":data.pkl", &consumed);
        if (!state_dict.ok()) {
            // A truncated buffer says so; keep waiting for more bytes. A real
            // parser error is final.
            if (state_dict.message().find("truncated pickle") != std::string::npos ||
                state_dict.message().find("ended without STOP") != std::string::npos) {
                return Status::success();
            }
            return fail(state_dict.message());
        }
        if (consumed == 0 || consumed > pickle.size()) {
            return fail("data.pkl ended at an invalid offset");
        }
        descriptor_bytes = consumed;
        descriptor_crc = mz_crc32(MZ_CRC32_INIT, pickle.data(), static_cast<size_t>(consumed));
        // Bytes after STOP in this buffer (the descriptor and possibly the next
        // entries) are still to be parsed.
        std::vector<uint8_t> extra(pickle.begin() + static_cast<std::ptrdiff_t>(consumed), pickle.end());
        pickle.resize(consumed);
        Status status = plan_and_begin(state_dict.value());
        if (!status.ok()) return status;
        log_line("stream: data.pkl read, " + std::to_string(records.size()) + " tensor(s), " +
                 std::to_string(kept_count) + " kept, " + std::to_string(dropped_count) + " dropped");
        if (!extra.empty()) {
            bytes.insert(bytes.end(), extra.begin(), extra.end());
        }
        state = has_descriptor ? State::descriptor : State::local_header;
        return Status::success();
    }

    // ---- the entry state machine ------------------------------------------
    Status read_local_header() {
        if (available() < kZipLocalHeaderBytes) return Status::success();
        const uint8_t* header = cursor();
        if (zip_little_u32(header) != kZipLocalHeaderSignature) {
            return fail("the checkpoint has no local zip header at stream offset " +
                        std::to_string(total_in - available()));
        }
        const ZipLocalHeader local = parse_zip_local_header(header);
        if ((local.flags & kZipFlagEncrypted) != 0) {
            return fail("the checkpoint's zip entry is encrypted");
        }
        if (local.method != 0) {
            return fail("the checkpoint's zip entry is compressed (method " +
                        std::to_string(local.method) +
                        "); the streaming converter reads stored entries only");
        }
        if (local.name_bytes > kMaxMetadataBytes) {
            return fail("the checkpoint has a corrupt zip local header");
        }
        const size_t header_bytes = kZipLocalHeaderBytes + local.name_bytes + local.extra_bytes;
        if (available() < header_bytes) return Status::success();
        name.assign(reinterpret_cast<const char*>(header + kZipLocalHeaderBytes), local.name_bytes);
        consume(header_bytes);

        has_descriptor = (local.flags & kZipFlagDataDescriptor) != 0;
        entry_bytes = has_descriptor ? 0 : local.compressed_bytes;
        entry_done = 0;
        entry_crc = MZ_CRC32_INIT;
        entry_kept = false;
        fp16.reset();
        fp32.reset();

        if (prefix.empty()) {
            if (!ends_with(name, "/data.pkl")) {
                return fail("the first zip entry is '" + name +
                            "', not <folder>/data.pkl; the streaming converter needs the pickle first");
            }
            prefix = name.substr(0, name.size() - std::string("data.pkl").size());
            pickle.clear();
            next_pickle_attempt = 1024;
            state = State::pickle;
            return Status::success();
        }
        if (name == prefix + "data.pkl") return fail("the checkpoint has more than one data.pkl");
        if (name.rfind(prefix + "data/", 0) == 0) {
            const std::string key = name.substr(prefix.size() + 5);
            const auto found = by_key.find(key);
            if (found == by_key.end()) {
                return fail("the checkpoint has a storage entry '" + name +
                            "' that data.pkl does not describe");
            }
            if (next_plan >= plan.size()) {
                return fail("the checkpoint has more storage entries than data.pkl describes (" + name + ")");
            }
            if (key != plan[next_plan]) {
                return fail("storage entry '" + name + "' arrived where '" + prefix + "data/" +
                            plan[next_plan] +
                            "' was expected; the checkpoint's entry order is not the one this build "
                            "reads (sorted storage keys)");
            }
            const Record& record = records[found->second];
            current_name = record.tensor.name;
            entry_bytes = record.entry_bytes;
            entry_skip = static_cast<uint64_t>(record.tensor.storage_offset) * 4;
            entry_want = record.tensor.numel() * 4;
            entry_kept = record.kept;
            if (!has_descriptor && local.compressed_bytes != entry_bytes) {
                return fail("storage entry '" + name + "' is " +
                            std::to_string(local.compressed_bytes) + " bytes, data.pkl says " +
                            std::to_string(entry_bytes));
            }
            if (entry_kept) {
                const bool as_fp16 = !options.keep_fp32 && record.tensor.shape.size() >= 2;
                if (as_fp16) {
                    fp16 = std::make_unique<Fp16Streamer>(writer);
                    const Status status = fp16->begin();
                    if (!status.ok()) return fail(status.message());
                } else {
                    fp32 = std::make_unique<PassthroughStreamer>(writer);
                    const Status status = fp32->begin();
                    if (!status.ok()) return fail(status.message());
                }
            }
            state = State::tensor;
            return Status::success();
        }

        // A torch metadata entry: byteorder, .format_version,
        // .storage_alignment, version, .data/serialization_id. Only byteorder
        // says anything the conversion needs.
        state = State::metadata;
        return Status::success();
    }

    Status on_pickle_data() {
        // Copy everything available: the parser needs a contiguous pickle and
        // retries are cheap (305 KB on the real file).
        if (available() > 0) {
            pickle.insert(pickle.end(), cursor(), cursor() + available());
            consume(available());
        }
        if (pickle.size() > kMaxPickleBytes) {
            return fail("data.pkl is larger than " + std::to_string(kMaxPickleBytes) + " bytes");
        }
        // A 1-byte feed would otherwise parse a growing pickle on every byte.
        // Retry at doubling sizes (1 KiB, 2 KiB, ...): the real 305 KB pickle
        // needs about ten attempts, and the total work stays one parse per size.
        if (pickle.size() < next_pickle_attempt) return Status::success();
        const Status status = on_pickle();
        if (state == State::pickle) {
            next_pickle_attempt = std::min(kMaxPickleBytes, pickle.size() * 2);
        }
        return status;
    }

    Status on_metadata() {
        // A fixed-size metadata entry (no data descriptor) can be skipped by its
        // header size.
        if (!has_descriptor && entry_bytes != 0) {
            if (available() < entry_bytes) return Status::success();
            Status status = metadata_payload(cursor(), static_cast<size_t>(entry_bytes));
            if (!status.ok()) return status;
            consume(static_cast<size_t>(entry_bytes));
            state = State::local_header;
            return Status::success();
        }
        // Otherwise find the descriptor + next local header. The payload is a
        // few bytes of ASCII, so a bounds-checked scan is exact; the descriptor's
        // CRC and sizes must agree with what was scanned.
        const size_t limit =
            std::min(available(), kMaxMetadataBytes + kDescriptorBytes + kZipLocalHeaderBytes);
        for (size_t i = kDescriptorBytes; i + kZipLocalHeaderBytes <= limit; ++i) {
            const uint8_t* p = cursor();
            if (zip_little_u32(p + i) != kZipLocalHeaderSignature) continue;
            const uint8_t* descriptor = p + i - kDescriptorBytes;
            if (std::memcmp(descriptor, kDescriptorSignature, 4) != 0) continue;
            const uint32_t crc = zip_little_u32(descriptor + 4);
            const uint32_t compressed = zip_little_u32(descriptor + 8);
            const uint32_t uncompressed = zip_little_u32(descriptor + 12);
            const size_t payload = i - kDescriptorBytes;
            if (compressed != payload || uncompressed != payload) continue;
            if (mz_crc32(MZ_CRC32_INIT, p, payload) != crc) continue;
            Status status = metadata_payload(p, payload);
            if (!status.ok()) return status;
            consume(i);
            state = State::local_header;
            return Status::success();
        }
        if (available() >= kMaxMetadataBytes + kDescriptorBytes + kZipLocalHeaderBytes) {
            return fail("a metadata entry of the checkpoint is larger than " +
                        std::to_string(kMaxMetadataBytes) + " bytes; not a torch.save layout this reads");
        }
        return Status::success();
    }

    Status metadata_payload(const uint8_t* data, size_t size) {
        if (ends_with(name, "/byteorder")) {
            if (size != 6 || std::memcmp(data, "little", 6) != 0) {
                return fail("checkpoint byteorder is not 'little'; only little-endian is read");
            }
        }
        return Status::success();
    }

    Status on_tensor() {
        if (entry_done < entry_bytes) {
            const size_t n = static_cast<size_t>(
                std::min<uint64_t>(available(), entry_bytes - entry_done));
            if (n == 0) return Status::success();
            const uint8_t* chunk = cursor();
            entry_crc = mz_crc32(entry_crc, chunk, n);

            if (entry_kept) {
                const uint64_t chunk_begin = entry_done;
                const uint64_t chunk_end = entry_done + n;
                const uint64_t from = std::max(chunk_begin, entry_skip);
                const uint64_t to = std::min(chunk_end, entry_skip + entry_want);
                if (from < to) {
                    const size_t offset = static_cast<size_t>(from - chunk_begin);
                    const size_t size = static_cast<size_t>(to - from);
                    const bool accepted = fp16 ? (*fp16)(from - entry_skip, chunk + offset, size)
                                               : (*fp32)(from - entry_skip, chunk + offset, size);
                    if (!accepted) {
                        return fail("tensor '" + current_name + "': the GGUF writer refused its bytes");
                    }
                }
            }
            consume(n);
            entry_done += n;
        }
        if (entry_done < entry_bytes) return Status::success();

        if (entry_kept) {
            Status status = fp16 ? fp16->finish() : fp32->finish();
            if (!status.ok()) return fail(status.message());
            fp16.reset();
            fp32.reset();
        }
        descriptor_bytes = entry_bytes;
        descriptor_crc = entry_crc;
        if (!name.empty() && name.rfind(prefix + "data/", 0) == 0) {
            ++tensors_done;
            ++next_plan;
            last_tensor_done = next_plan == plan.size();
            maybe_log();
        }
        // The last storage's descriptor is validated like every other one; only
        // then does the archive's tail begin.
        state = has_descriptor ? State::descriptor
                               : (last_tensor_done ? State::tail : State::local_header);
        return Status::success();
    }

    Status on_descriptor() {
        if (available() < kDescriptorBytes) return Status::success();
        const uint8_t* descriptor = cursor();
        if (std::memcmp(descriptor, kDescriptorSignature, 4) != 0) {
            // A writer without the data-descriptor flag puts the next local
            // header right here.
            if (!has_descriptor) {
                state = State::local_header;
                return Status::success();
            }
            return fail("the checkpoint has no data descriptor after '" + name + "'");
        }
        const uint32_t crc = zip_little_u32(descriptor + 4);
        const uint32_t compressed = zip_little_u32(descriptor + 8);
        const uint32_t uncompressed = zip_little_u32(descriptor + 12);
        if (compressed != descriptor_bytes || uncompressed != descriptor_bytes) {
            return fail("the data descriptor of '" + name + "' says " +
                        std::to_string(compressed) + "/" + std::to_string(uncompressed) +
                        " bytes, expected " + std::to_string(descriptor_bytes));
        }
        if (crc != static_cast<uint32_t>(descriptor_crc)) {
            return fail("zip entry '" + name + "' failed its CRC check");
        }
        consume(kDescriptorBytes);
        state = last_tensor_done ? State::tail : State::local_header;
        return Status::success();
    }

    // Keeps the last kTailWindowBytes of the stream: finish() checks that the
    // central directory ends with the zip end record.
    void remember_tail(size_t count) {
        if (count == 0) return;
        const size_t keep = std::min(count, kTailWindowBytes);
        tail_bytes.insert(tail_bytes.end(), cursor() + (count - keep), cursor() + count);
        if (tail_bytes.size() > kTailWindowBytes) {
            tail_bytes.erase(tail_bytes.begin(),
                             tail_bytes.begin() +
                                 static_cast<std::ptrdiff_t>(tail_bytes.size() - kTailWindowBytes));
        }
    }

    // "PK\x05\x06", 18 more bytes, then the archive's comment: every zip ends
    // with it, zip64 included. The tail of the stream (the last descriptor, the
    // torch metadata and the central directory) is otherwise dropped, so this is
    // what says the stream ended with a directory instead of arbitrary bytes.
    Status check_end_record() const {
        constexpr uint8_t kEndSignature[4] = {'P', 'K', 0x05, 0x06};
        constexpr size_t kEndRecordBytes = 22;
        if (tail_bytes.size() < kEndRecordBytes) {
            return Status::error("the checkpoint stream ended without a zip end record");
        }
        for (size_t i = tail_bytes.size() - kEndRecordBytes + 1; i-- > 0;) {
            if (std::memcmp(tail_bytes.data() + i, kEndSignature, 4) != 0) continue;
            const uint32_t comment = zip_little_u16(tail_bytes.data() + i + 20);
            if (tail_bytes.size() - i - kEndRecordBytes == comment) return Status::success();
        }
        return Status::error("the checkpoint stream ended without a zip end record");
    }

    Status pump() {
        for (;;) {
            const size_t before_at = at;
            const size_t before_size = bytes.size();
            const State before_state = state;
            Status status = Status::success();
            switch (state) {
                case State::local_header: status = read_local_header(); break;
                case State::pickle: status = on_pickle_data(); break;
                case State::metadata: status = on_metadata(); break;
                case State::tensor: status = on_tensor(); break;
                case State::descriptor: status = on_descriptor(); break;
                case State::tail:
                    remember_tail(available());
                    consume(available());
                    return Status::success();
                case State::failed:
                    return Status::error(failure);
                case State::done:
                    return Status::error("the streaming converter already finished");
            }
            if (!status.ok()) return status;
            if (state == State::failed) return Status::error(failure);
            // Nothing was consumed and no state changed: this handler needs
            // more bytes before it can make progress.
            if (at == before_at && bytes.size() == before_size && state == before_state) {
                return Status::success();
            }
        }
    }
};

StreamConverter::StreamConverter(Path output, StreamConvertOptions options)
    : impl_(std::make_unique<Impl>(std::move(output), std::move(options))) {}

StreamConverter::~StreamConverter() { abort(); }

Status StreamConverter::feed(const void* data, size_t size) {
    if (impl_->state == Impl::State::failed) return Status::error(impl_->failure);
    if (impl_->state == Impl::State::done) {
        return Status::error("the streaming converter already finished");
    }
    if (size == 0) return Status::success();
    impl_->hasher.update(data, size);
    impl_->total_in += size;
    if (impl_->options.source_bytes != 0 && impl_->total_in > impl_->options.source_bytes) {
        return impl_->fail("the stream is longer than the pinned " +
                           std::to_string(impl_->options.source_bytes) + " bytes");
    }
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    impl_->bytes.insert(impl_->bytes.end(), bytes, bytes + size);
    const Status status = impl_->pump();
    if (status.ok()) impl_->maybe_log();
    return status;
}

Result<ConvertStats> StreamConverter::finish() {
    if (impl_->state == Impl::State::failed) return Result<ConvertStats>::failure(impl_->failure);
    if (impl_->state != Impl::State::tail) {
        return Result<ConvertStats>::failure(
            "the stream ended after " + std::to_string(impl_->total_in) +
            " bytes, before all tensors arrived (" + std::to_string(impl_->next_plan) + " of " +
            std::to_string(impl_->plan.size()) + " storages)");
    }
    if (impl_->total_in != 0 && impl_->options.source_bytes != 0 &&
        impl_->total_in != impl_->options.source_bytes) {
        return Result<ConvertStats>::failure(
            "the stream carried " + std::to_string(impl_->total_in) + " bytes, expected " +
            std::to_string(impl_->options.source_bytes));
    }
    const Status ended = impl_->check_end_record();
    if (!ended.ok()) return Result<ConvertStats>::failure(ended.message());
    impl_->digest = Sha256::hex(impl_->hasher.finish());
    Status status = impl_->writer.patch_string("sharp.source_sha256", impl_->digest);
    if (!status.ok()) return Result<ConvertStats>::failure(status.message());
    status = impl_->writer.finish();
    if (!status.ok()) return Result<ConvertStats>::failure(status.message());

    ConvertStats stats;
    stats.source_sha256 = impl_->digest;
    stats.source_bytes = impl_->options.source_bytes != 0 ? impl_->options.source_bytes : impl_->total_in;
    stats.source_tensor_bytes = impl_->tensor_bytes_in;
    stats.gguf_bytes = impl_->writer.bytes_written();
    stats.tensor_count = impl_->kept_count;
    stats.dropped_count = impl_->dropped_count;
    stats.data_offset = impl_->data_offset;
    stats.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - impl_->started).count();
    impl_->state = Impl::State::done;

    if (impl_->options.log) {
        impl_->log_line("stream: " + (impl_->options.source_filename.empty()
                                          ? std::string("checkpoint")
                                          : impl_->options.source_filename) +
                        " done, " + std::to_string(stats.source_bytes) + " bytes in " +
                        seconds_text(stats.seconds) + " s (" +
                        rate_text(stats.source_bytes, stats.seconds) + "), " +
                        std::to_string(stats.gguf_bytes) + " GGUF bytes (" +
                        rate_text(stats.gguf_bytes, stats.seconds) + "), " +
                        std::to_string(stats.tensor_count) + " tensors, sha256 " + stats.source_sha256);
    }
    return Result<ConvertStats>::success(std::move(stats));
}

void StreamConverter::abort() {
    if (impl_ == nullptr) return;
    if (impl_->state == Impl::State::done) return;  // the manager owns the finished file
    remove_if_present(impl_->out);
}

}  // namespace splat
