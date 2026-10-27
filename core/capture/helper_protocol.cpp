// W5. Shared file-mailbox protocol (see helper_protocol.h). Host and payload
// code: std::filesystem + nlohmann json only, no sce calls, no exceptions on
// bad input (JSON is parsed with allow_exceptions=false).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "capture/helper_protocol.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include <sys/stat.h>

namespace splat {
namespace {

std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool is_hex8(const std::string& text) {
    if (text.size() != 8) return false;
    for (char c : text) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

bool all_digits(const std::string& text, std::size_t from, std::size_t count) {
    if (from + count > text.size()) return false;
    for (std::size_t i = from; i < from + count; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) return false;
    }
    return true;
}

// "20261025_052805" out of a capture stem, or "" when the name does not carry
// a capture timestamp.
std::string time_from_stem(const std::string& stem) {
    if (stem.size() < 15) return std::string();
    if (!all_digits(stem, 0, 8) || stem[8] != '_' || !all_digits(stem, 9, 6)) return std::string();
    return stem.substr(0, 15);
}

std::string format_time(std::time_t value) {
    std::tm tm{};
#if defined(_WIN32)
    if (gmtime_s(&tm, &value) != 0) return std::string();
#else
    if (gmtime_r(&value, &tm) == nullptr) return std::string();
#endif
    char text[32];
    std::snprintf(text, sizeof(text), "%04d%02d%02d_%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return text;
}

std::string time_of(const Path& path) {
    const std::string from_name = time_from_stem(path.stem().string());
    if (!from_name.empty()) return from_name;
    struct stat info {};
    if (::stat(path.c_str(), &info) == 0) return format_time(info.st_mtime);
    return std::string();
}

// The 8 hex digits of a capture name; a stable fallback for other names so a
// copy still gets a unique-enough destination.
std::string hash_of(const Path& path) {
    const std::string stem = path.stem().string();
    const std::size_t underscore = stem.rfind('_');
    if (underscore != std::string::npos && is_hex8(stem.substr(underscore + 1))) {
        return lowercase(stem.substr(underscore + 1));
    }
    std::uint32_t hash = 2166136261u;  // FNV-1a, identical on host and console
    for (char c : path.string()) {
        hash ^= static_cast<std::uint32_t>(static_cast<unsigned char>(c));
        hash *= 16777619u;
    }
    char text[9];
    std::snprintf(text, sizeof(text), "%08x", hash);
    return text;
}

// Component-wise: "/a/bc" is not inside "/a/b".
bool is_within(const Path& child, const Path& parent) {
    auto child_part = child.begin();
    for (auto parent_part = parent.begin(); parent_part != parent.end(); ++parent_part, ++child_part) {
        if (child_part == child.end() || *child_part != *parent_part) return false;
    }
    return true;
}

// Symlink-free absolute form of `path`. std::filesystem::canonical() is
// realpath(3): it resolves every component and fails on one that is missing or
// unreadable.
bool resolved(const Path& path, Path& out) {
    std::error_code error;
    out = std::filesystem::canonical(path, error);
    return !error && !out.empty();
}

// True when `path` or a component between it and `root` is a symlink, or when
// the type cannot be asked (refusing is the safe direction). `root` itself is
// not checked: it is the configured capture root, and both /var/folders on the
// host and the mount point behind /user/av_contents are links.
bool symlink_below(const Path& root, const Path& path) {
    auto component = path.begin();
    for (auto root_part = root.begin(); root_part != root.end(); ++root_part, ++component) {
        if (component == path.end()) return true;  // not below the root after all
    }
    Path current = root;
    for (; component != path.end(); ++component) {
        if (component->empty()) continue;
        current /= *component;
        std::error_code error;
        const std::filesystem::file_status status = std::filesystem::symlink_status(current, error);
        if (error || std::filesystem::is_symlink(status)) return true;
    }
    return false;
}

void append_error(HelperResult& result, const std::string& message) {
    if (!result.error.empty()) result.error += "; ";
    result.error += message;
    result.ok = false;
}

std::string trim(std::string text) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
    return text;
}

std::string read_text_file(const Path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::string();
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

CaptureInfo make_capture(const Path& path, const std::string& format) {
    CaptureInfo info;
    info.path = path;
    info.name = path.filename().string();
    info.time = time_of(path);
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    info.bytes = error ? 0 : static_cast<std::uint64_t>(size);
    info.fmt = format;
    info.supported = format != "jxr";
    return info;
}

}  // namespace

std::string capture_format(const Path& path) {
    const std::string extension = lowercase(path.extension().string());
    if (extension == ".jpg" || extension == ".jpeg") return "jpg";
    if (extension == ".png") return "png";
    if (extension == ".jxr") return "jxr";
    return std::string();
}

bool is_supported_capture(const Path& path) {
    const std::string format = capture_format(path);
    return format == "jpg" || format == "png";
}

HelperRequest parse_request(const std::string& text) {
    HelperRequest request;

    const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        request.error = "malformed request: not a JSON object";
        return request;
    }
    if (!root.contains("id") || !root["id"].is_number_integer()) {
        request.error = "malformed request: missing integer id";
        return request;
    }
    request.id = root["id"].get<long long>();

    if (!root.contains("op") || !root["op"].is_string()) {
        request.error = "malformed request: missing op";
        return request;
    }
    const std::string op = root["op"].get<std::string>();
    if (op == "list") {
        request.op = HelperOp::List;
        return request;
    }
    if (op != "copy") {
        request.error = "malformed request: unknown op '" + op + "'";
        return request;
    }

    request.op = HelperOp::Copy;
    if (!root.contains("paths") || !root["paths"].is_array()) {
        request.error = "malformed request: copy needs a paths array";
        return request;
    }
    for (const nlohmann::json& entry : root["paths"]) {
        if (!entry.is_string()) {
            request.error = "malformed request: paths must be strings";
            return request;
        }
        request.paths.emplace_back(entry.get<std::string>());
    }
    return request;
}

std::vector<CaptureInfo> scan_captures(const Path& root) {
    std::vector<CaptureInfo> captures;

    const Path photo = root / "photo";
    std::error_code error;
    if (!std::filesystem::is_directory(photo, error)) return captures;

    std::filesystem::recursive_directory_iterator it(
        photo, std::filesystem::directory_options::skip_permission_denied, error);
    if (error) return captures;
    const std::filesystem::recursive_directory_iterator end;

    while (it != end) {
        const std::filesystem::directory_entry& entry = *it;
        std::error_code entry_error;
        if (entry.is_directory(entry_error)) {
            const std::string name = entry.path().filename().string();
            if (name == "thumbnails" || it.depth() >= kHelperMaxScanDepth) it.disable_recursion_pending();
        } else if (!entry_error && entry.is_regular_file(entry_error)) {
            const std::string format = capture_format(entry.path());
            if (!format.empty()) captures.push_back(make_capture(entry.path(), format));
        }
        it.increment(error);
        if (error) break;
    }

    std::sort(captures.begin(), captures.end(), [](const CaptureInfo& a, const CaptureInfo& b) {
        if (a.time != b.time) return a.time > b.time;
        return a.name > b.name;
    });
    if (captures.size() > kHelperMaxCaptures) captures.resize(kHelperMaxCaptures);
    return captures;
}

HelperResult copy_captures(const Path& root, const Path& inbox, const std::vector<Path>& paths) {
    HelperResult result;

    // Realpath, not a lexical comparison: this is the helper payload, where
    // canonical() (realpath) and symlink_status() (lstat) answer. A PS5 title is
    // refused both (BACKLOG, W17), so a root that cannot be resolved refuses the
    // copy instead of falling back to a lexical test that a symlink defeats.
    Path capture_root;  // absolute and lexical: the base the components are walked from
    if (!normal_absolute(root, capture_root)) {
        result.ok = false;
        result.error = "cannot resolve capture root";
        return result;
    }
    Path real_root;  // the same root with every link resolved
    if (!resolved(capture_root, real_root)) {
        result.ok = false;
        result.error = "cannot resolve capture root";
        return result;
    }
    Path inbox_dir;
    if (!normal_absolute(inbox, inbox_dir)) {
        result.ok = false;
        result.error = "cannot resolve the inbox";
        return result;
    }

    for (const Path& requested : paths) {
        Path path;
        if (!normal_absolute(requested, path)) {
            append_error(result, "cannot resolve " + requested.string());
            continue;
        }
        if (!is_within(path, capture_root)) {
            append_error(result, "refused outside capture root: " + requested.string());
            continue;
        }
        // No symlink on the way in, even one that points back inside the root.
        if (symlink_below(capture_root, path)) {
            append_error(result, "refused symlink in path: " + requested.string());
            continue;
        }
        // The resolved file must still be inside the resolved root: with the
        // component check above this is the second half of the same rule.
        Path real_path;
        if (!resolved(path, real_path)) {
            append_error(result, "cannot resolve " + requested.string());
            continue;
        }
        if (!is_within(real_path, real_root)) {
            append_error(result, "refused outside capture root: " + requested.string());
            continue;
        }
        std::error_code path_error;
        if (!std::filesystem::is_regular_file(real_path, path_error)) {
            append_error(result, "not a file: " + requested.string());
            continue;
        }
        const std::string format = capture_format(real_path);
        if (format.empty()) {
            append_error(result, "not a capture: " + requested.string());
            continue;
        }
        if (format == "jxr") {
            append_error(result, "HDR capture not supported: " + requested.string());
            continue;
        }

        std::string time = time_of(real_path);
        if (time.empty()) time = "00000000_000000";
        const Path destination =
            inbox_dir / ("capture-" + time + "-" + hash_of(real_path) + "." + format);
        // The name is derived here, so it can only land directly in the inbox;
        // anything else is refused rather than written outside it.
        if (destination.parent_path() != inbox_dir) {
            append_error(result, "refused outside the inbox: " + requested.string());
            continue;
        }

        std::filesystem::create_directories(inbox_dir, path_error);
        if (path_error) {
            append_error(result, "cannot create inbox: " + path_error.message());
            continue;
        }
        // "Never overwrite": an existing destination is skipped, not replaced.
        if (std::filesystem::exists(destination, path_error)) continue;
        std::filesystem::copy_file(real_path, destination, std::filesystem::copy_options::none, path_error);
        if (path_error) {
            append_error(result, "copy failed: " + requested.string() + ": " + path_error.message());
            continue;
        }
        // src is the path the title named, not the resolved one, so the UI can
        // match it against the capture entry it showed.
        result.copied.push_back(CopiedFile{requested, destination});
    }
    return result;
}

std::string format_response(long long id, const HelperResult& result) {
    nlohmann::ordered_json root;
    root["id"] = id;
    root["ok"] = result.ok;

    nlohmann::ordered_json captures = nlohmann::ordered_json::array();
    for (const CaptureInfo& capture : result.captures) {
        nlohmann::ordered_json entry;
        entry["path"] = capture.path.string();
        entry["name"] = capture.name;
        entry["time"] = capture.time;
        entry["bytes"] = capture.bytes;
        entry["fmt"] = capture.fmt;
        entry["supported"] = capture.supported;
        captures.push_back(std::move(entry));
    }
    root["captures"] = std::move(captures);

    nlohmann::ordered_json copied = nlohmann::ordered_json::array();
    for (const CopiedFile& file : result.copied) {
        nlohmann::ordered_json entry;
        entry["src"] = file.src.string();
        entry["dst"] = file.dst.string();
        copied.push_back(std::move(entry));
    }
    root["copied"] = std::move(copied);
    root["error"] = result.error;

    // replace: a capture file name that is not UTF-8 must not throw.
    return root.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
}

std::string handle_request(const std::string& text, const Path& root, const Path& inbox) {
    const HelperRequest request = parse_request(text);
    HelperResult result;
    if (!request.valid()) {
        result.ok = false;
        result.error = request.error;
        return format_response(request.id, result);
    }
    if (request.op == HelperOp::List) {
        result.captures = scan_captures(root);
    } else {
        result = copy_captures(root, inbox, request.paths);
    }
    return format_response(request.id, result);
}

Path helper_root(const Path& helper_dir) {
    const std::string text = trim(read_text_file(helper_dir / kHelperRootName));
    if (!text.empty()) {
        const Path candidate(text);
        if (candidate.is_absolute()) return candidate;
    }
    return Path(kDefaultCaptureRoot);
}

}  // namespace splat
