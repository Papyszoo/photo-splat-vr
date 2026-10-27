#include "model/zip_archive.h"

#include <cstddef>
#include <cstring>

extern "C" {
#include "miniz/miniz.h"
#include "miniz/miniz_zip.h"
}

namespace splat {
namespace {

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
        impl_->entries.push_back(std::move(entry));
    }
    return Status::success();
}

void ZipArchive::close() {
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

}  // namespace splat
