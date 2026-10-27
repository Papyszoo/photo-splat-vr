#include "ingest/upload_store.h"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <sstream>

#include <fcntl.h>
#include <unistd.h>

namespace splat {
namespace {

bool starts_with(const std::string& data, const unsigned char* magic, size_t size) {
    if (data.size() < size) return false;
    return std::memcmp(data.data(), magic, size) == 0;
}

const unsigned char kJpegMagic[] = {0xff, 0xd8, 0xff};
const unsigned char kPngMagic[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};

std::string lowercase(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// The stem without a known photo extension, so the stored name does not grow
// IMG_0001.jpg-...jpg. The extension is chosen from the sniffed format anyway.
std::string stem_of(const std::string& safe_name) {
    const std::string lowered = lowercase(safe_name);
    for (const char* extension : {".jpeg", ".jpg", ".png"}) {
        const size_t length = std::strlen(extension);
        if (lowered.size() > length && lowered.compare(lowered.size() - length, length, extension) == 0) {
            return safe_name.substr(0, safe_name.size() - length);
        }
    }
    return safe_name;
}

std::string random_suffix() {
    std::random_device device;
    std::mt19937_64 rng((static_cast<uint64_t>(device()) << 32) ^ device());
    std::ostringstream out;
    out << std::hex << (rng() & 0xffffffffull);
    return out.str();
}

bool write_all(int fd, const char* data, size_t size) {
    size_t written = 0;
    while (written < size) {
        const ssize_t result = ::write(fd, data + written, size - written);
        if (result < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        written += static_cast<size_t>(result);
    }
    return true;
}

// Best effort: makes the rename survive a power cut, not just a crash. A
// filesystem that cannot sync a directory still gets the atomic rename.
void sync_directory(const Path& directory) {
    const int fd = ::open(directory.string().c_str(), O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
}

}  // namespace

UploadStore::UploadStore(Path inbox, uint64_t max_bytes)
    : inbox_(std::move(inbox)), max_bytes_(max_bytes) {}

std::optional<UploadStore::Format> UploadStore::detect_format(const std::string& data) {
    if (starts_with(data, kPngMagic, sizeof(kPngMagic))) return Format::Png;
    if (starts_with(data, kJpegMagic, sizeof(kJpegMagic))) return Format::Jpeg;
    return std::nullopt;
}

const char* UploadStore::extension_for(Format format) {
    return format == Format::Png ? ".png" : ".jpg";
}

std::string UploadStore::safe_filename(const std::string& original) {
    if (original.empty() || original == "." || original == "..") return {};
    if (original.find('/') != std::string::npos) return {};
    if (original.find('\\') != std::string::npos) return {};

    std::string cleaned;
    cleaned.reserve(original.size());
    for (const unsigned char c : original) {
        if (std::isalnum(c) != 0 || c == '.' || c == '_' || c == '-') {
            cleaned.push_back(static_cast<char>(c));
        } else {
            cleaned.push_back('_');
        }
    }
    // A leading dot would hide the file from the watcher's extension filter on
    // some tools; the inbox is user-visible, so refuse it instead.
    if (cleaned.empty() || cleaned.front() == '.') return {};
    if (cleaned.size() > 128) cleaned.resize(128);
    return cleaned;
}

Result<std::string> UploadStore::store(const std::string& original_name, const std::string& data) {
    if (data.empty()) {
        return Result<std::string>::failure("empty upload");
    }
    const std::optional<Format> format = detect_format(data);
    if (!format) {
        return Result<std::string>::failure("not a JPEG or PNG image (magic bytes do not match)");
    }
    if (data.size() > max_bytes_) {
        return Result<std::string>::failure("image is larger than the " + std::to_string(max_bytes_) +
                                            " byte limit");
    }

    const std::string safe_name = safe_filename(original_name);
    if (safe_name.empty()) {
        return Result<std::string>::failure("unsafe file name");
    }

    std::error_code error;
    std::filesystem::create_directories(inbox_, error);
    if (error) {
        return Result<std::string>::failure("cannot create inbox " + inbox_.string() + ": " +
                                            error.message());
    }

    std::string stem = stem_of(safe_name);
    if (stem.empty()) stem = "photo";

    // The timestamp keeps names unique across restarts; the counter covers two
    // uploads of the same photo within the same millisecond.
    const uint64_t stamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    Path final_path;
    std::string final_name;
    for (int attempt = 0; attempt < 1000; ++attempt) {
        std::ostringstream name;
        name << stem << "-" << stamp << "-" << counter_.fetch_add(1) << extension_for(*format);
        final_name = name.str();
        final_path = inbox_ / final_name;
        error.clear();
        const bool exists = std::filesystem::exists(final_path, error);
        if (!error && !exists) break;
        final_name.clear();
    }
    if (final_name.empty()) {
        return Result<std::string>::failure("cannot find a free file name in " + inbox_.string());
    }

    // Same directory as the final file, so rename() is atomic and the temp name
    // has a .part extension the watcher ignores even if a scan races the write.
    const Path temp_path = inbox_ / (".upload-" + random_suffix() + ".part");
    const int fd = ::open(temp_path.string().c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        return Result<std::string>::failure("cannot create " + temp_path.string() + ": " +
                                            std::strerror(errno));
    }

    bool ok = write_all(fd, data.data(), data.size());
    if (ok && ::fsync(fd) != 0) ok = false;
    if (::close(fd) != 0) ok = false;
    if (!ok) {
        ::unlink(temp_path.string().c_str());
        return Result<std::string>::failure("failed while writing " + temp_path.string());
    }

    if (::rename(temp_path.string().c_str(), final_path.string().c_str()) != 0) {
        const std::string message = "cannot publish " + final_name + ": " + std::strerror(errno);
        ::unlink(temp_path.string().c_str());
        return Result<std::string>::failure(message);
    }
    sync_directory(inbox_);
    return Result<std::string>::success(final_name);
}

}  // namespace splat
