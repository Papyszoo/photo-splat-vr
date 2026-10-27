#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

#include "common/path.h"
#include "common/status.h"

namespace splat {

// C9. Accepts an uploaded photo after checking its magic bytes, its size and
// its name, then publishes it in the inbox with a temp file + rename. The
// InboxWatcher only sees the final name, never a partially written file.
class UploadStore {
public:
    static constexpr uint64_t kDefaultMaxBytes = 40ull * 1024ull * 1024ull;  // 40 MB

    enum class Format { Jpeg, Png };

    explicit UploadStore(Path inbox, uint64_t max_bytes = kDefaultMaxBytes);

    // Content sniffing, not the extension: JPEG starts with FF D8 FF, PNG with
    // the 8-byte signature.
    static std::optional<Format> detect_format(const std::string& data);
    static const char* extension_for(Format format);

    // Returns a plain file name (no directory parts) or an empty string when
    // the name is unsafe: path separators, "..", a leading dot or no name at
    // all. Other bytes are replaced with '_'.
    static std::string safe_filename(const std::string& original);

    // Writes the photo into the inbox atomically and returns the stored file
    // name (the extension comes from the detected format). Fails on unsupported
    // content, an empty/oversized body, an unsafe name or an I/O error.
    Result<std::string> store(const std::string& original_name, const std::string& data);

    const Path& inbox() const { return inbox_; }
    uint64_t max_bytes() const { return max_bytes_; }

private:
    Path inbox_;
    uint64_t max_bytes_;
    std::atomic<uint64_t> counter_{0};
};

}  // namespace splat
