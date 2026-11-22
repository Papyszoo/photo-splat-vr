#include "model/zip_archive.h"

#include <sys/types.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "miniz/miniz.h"
#include "miniz/miniz_zip.h"
}

namespace splat {
namespace {

// A stored entry is read from the raw file, not through miniz's 64 KiB extract
// buffer: the checkpoint's tensors are 2.6 GB of method-0 entries and the
// console's per-call cost is what the slow conversion was made of
// (runs/issue-1-crash/sharp-slow-report.md §1.4).
constexpr size_t kStoredReadBytes = 1u << 20;
constexpr uint32_t kLocalHeaderSignature = 0x04034b50u;
constexpr size_t kLocalHeaderBytes = 30;

uint32_t little_u16(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8);
}

uint32_t little_u32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

// Callback state for mz_zip_reader_extract_to_callback. miniz calls this with
// monotonically increasing offsets; we forward the chunks to the caller's sink.
struct SinkContext {
    const ZipSink* sink = nullptr;
    Status error;
    uint64_t bytes = 0;
};

// mz_file_write_func: return the number of bytes accepted, or 0 to abort.
size_t sink_callback(void* opaque, mz_uint64 file_ofs, const void* buffer, size_t size) {
    SinkContext* context = static_cast<SinkContext*>(opaque);
    if (!(*context->sink)(file_ofs, buffer, size)) {
        context->error = Status::error("zip entry sink refused data at offset " +
                                       std::to_string(file_ofs));
        return 0;
    }
    context->bytes += size;
    return size;
}

}  // namespace

struct ZipArchive::Impl {
    mz_zip_archive archive{};
    std::FILE* raw = nullptr;  // the same file, for the stored-entry fast path
    std::vector<ZipEntryInfo> entries;
    bool zip64 = false;
    bool open = false;
    Path path;
};

ZipArchive::ZipArchive() : impl_(std::make_unique<Impl>()) {}
ZipArchive::~ZipArchive() { close(); }

ZipArchive::ZipArchive(ZipArchive&& other) noexcept = default;
ZipArchive& ZipArchive::operator=(ZipArchive&& other) noexcept = default;

Status ZipArchive::open(const Path& path) {
    close();
    impl_ = std::make_unique<Impl>();
    impl_->path = path;

    std::memset(&impl_->archive, 0, sizeof(impl_->archive));
    if (!mz_zip_reader_init_file(&impl_->archive, path.string().c_str(), 0)) {
        return Status::error("cannot open zip archive " + path.string() + ": " +
                             mz_zip_get_error_string(mz_zip_get_last_error(&impl_->archive)));
    }
    impl_->open = true;
    impl_->zip64 = mz_zip_is_zip64(&impl_->archive) != 0;
    impl_->raw = std::fopen(path.string().c_str(), "rb");
    if (impl_->raw == nullptr) {
        close();
        return Status::error("cannot open " + path.string() + " for reading");
    }

    const mz_uint count = mz_zip_reader_get_num_files(&impl_->archive);
    impl_->entries.reserve(count);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&impl_->archive, i, &stat)) {
            return Status::error("cannot read zip directory entry " + std::to_string(i) + " of " +
                                 path.string());
        }
        ZipEntryInfo entry;
        entry.name = stat.m_filename;
        entry.uncompressed_bytes = stat.m_uncomp_size;
        entry.compressed_bytes = stat.m_comp_size;
        entry.method = stat.m_method;
        entry.crc32 = stat.m_crc32;
        entry.local_header_offset = stat.m_local_header_ofs;
        impl_->entries.push_back(std::move(entry));
    }
    return Status::success();
}

void ZipArchive::close() {
    if (impl_ && impl_->raw != nullptr) {
        std::fclose(impl_->raw);
        impl_->raw = nullptr;
    }
    if (impl_ && impl_->open) {
        mz_zip_reader_end(&impl_->archive);
        impl_->open = false;
    }
}

bool ZipArchive::is_open() const { return impl_ && impl_->open; }

bool ZipArchive::is_zip64() const { return impl_ && impl_->zip64; }

const std::vector<ZipEntryInfo>& ZipArchive::entries() const { return impl_->entries; }

int ZipArchive::find(const std::string& name) const {
    for (size_t i = 0; i < impl_->entries.size(); ++i) {
        if (impl_->entries[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

Result<std::vector<uint8_t>> ZipArchive::read_entry(const std::string& name, uint64_t max_bytes) {
    const int index = find(name);
    if (index < 0) return Result<std::vector<uint8_t>>::failure("no zip entry " + name);

    const ZipEntryInfo& entry = impl_->entries[static_cast<size_t>(index)];
    if (entry.uncompressed_bytes > max_bytes) {
        return Result<std::vector<uint8_t>>::failure("zip entry " + name + " is " +
                                                     std::to_string(entry.uncompressed_bytes) +
                                                     " bytes, over the " +
                                                     std::to_string(max_bytes) + " byte limit");
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(entry.uncompressed_bytes));
    size_t written = 0;
    const Status status = stream_entry(entry, [&](uint64_t, const void* data, size_t size) {
        if (written + size > bytes.size()) return false;
        std::memcpy(bytes.data() + written, data, size);
        written += size;
        return true;
    });
    if (!status.ok()) return Result<std::vector<uint8_t>>::failure(status.message());
    if (written != bytes.size()) {
        return Result<std::vector<uint8_t>>::failure("zip entry " + name + " ended after " +
                                                     std::to_string(written) + " of " +
                                                     std::to_string(bytes.size()) + " bytes");
    }
    return Result<std::vector<uint8_t>>::success(std::move(bytes));
}

Status ZipArchive::stream_entry(const std::string& name, const ZipSink& sink) {
    const int index = find(name);
    if (index < 0) return Status::error("no zip entry " + name);
    return stream_entry(impl_->entries[static_cast<size_t>(index)], sink);
}

Status ZipArchive::stream_entry(const ZipEntryInfo& entry, const ZipSink& sink) {
    if (!is_open()) return Status::error("zip archive is not open");

    const int index = find(entry.name);
    if (index < 0) return Status::error("no zip entry " + entry.name);
    if (entry.method == 0) return stream_stored(entry, sink);

    SinkContext context;
    context.sink = &sink;
    if (!mz_zip_reader_extract_to_callback(&impl_->archive, static_cast<mz_uint>(index),
                                           sink_callback, &context, 0)) {
        if (!context.error.ok()) return context.error;
        return Status::error("cannot extract zip entry " + entry.name + " from " +
                             impl_->path.string() + ": " +
                             mz_zip_get_error_string(mz_zip_get_last_error(&impl_->archive)));
    }
    if (context.bytes != entry.uncompressed_bytes) {
        return Status::error("zip entry " + entry.name + " yielded " +
                             std::to_string(context.bytes) + " of " +
                             std::to_string(entry.uncompressed_bytes) + " bytes");
    }
    return Status::success();
}

Status ZipArchive::stream_stored(const ZipEntryInfo& entry, const ZipSink& sink) {
    if (impl_->raw == nullptr) return Status::error("zip archive is not open");

    uint8_t header[kLocalHeaderBytes] = {};
    if (::fseeko(impl_->raw, static_cast<off_t>(entry.local_header_offset), SEEK_SET) != 0 ||
        std::fread(header, 1, sizeof(header), impl_->raw) != sizeof(header)) {
        return Status::error("cannot read the local header of " + entry.name + " in " +
                             impl_->path.string());
    }
    if (little_u32(header) != kLocalHeaderSignature) {
        return Status::error("zip entry " + entry.name + " has no local header");
    }
    if ((little_u16(header + 6) & 1u) != 0) {
        return Status::error("zip entry " + entry.name + " is encrypted");
    }
    const uint32_t name_len = little_u16(header + 26);
    const uint32_t extra_len = little_u16(header + 28);
    if (name_len > 4096) return Status::error("zip entry " + entry.name + " has a corrupt name length");
    std::string local_name(name_len, '\0');
    if (name_len > 0 &&
        std::fread(local_name.data(), 1, name_len, impl_->raw) != name_len) {
        return Status::error("cannot read the local name of " + entry.name);
    }
    if (local_name != entry.name) {
        return Status::error("zip entry " + entry.name + " has the local name '" + local_name + "'");
    }

    const uint64_t data_offset = entry.local_header_offset + kLocalHeaderBytes + name_len + extra_len;
    if (::fseeko(impl_->raw, static_cast<off_t>(data_offset), SEEK_SET) != 0) {
        return Status::error("cannot seek to the data of " + entry.name);
    }

    std::vector<uint8_t> buffer;
    buffer.reserve(static_cast<size_t>(std::min<uint64_t>(kStoredReadBytes, entry.uncompressed_bytes)));
    uint64_t offset = 0;
    mz_ulong crc = MZ_CRC32_INIT;
    while (offset < entry.uncompressed_bytes) {
        const size_t want = static_cast<size_t>(
            std::min<uint64_t>(kStoredReadBytes, entry.uncompressed_bytes - offset));
        buffer.resize(want);
        if (std::fread(buffer.data(), 1, want, impl_->raw) != want) {
            return Status::error("zip entry " + entry.name + " ended after " +
                                 std::to_string(offset) + " of " +
                                 std::to_string(entry.uncompressed_bytes) + " bytes");
        }
        crc = mz_crc32(crc, buffer.data(), want);
        if (!sink(offset, buffer.data(), want)) {
            return Status::error("zip entry sink refused data at offset " + std::to_string(offset));
        }
        offset += want;
    }
    if (static_cast<uint32_t>(crc) != entry.crc32) {
        return Status::error("zip entry " + entry.name + " failed its CRC check");
    }
    return Status::success();
}

}  // namespace splat
