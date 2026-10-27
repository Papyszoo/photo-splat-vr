#pragma once

// W5. File-mailbox protocol between the viewer title and the resident
// `splat-helper` payload. A title cannot see /user/av_contents (docs/research/
// browse-ui.md 1.3), a payload can (1.4 C), so the two talk through files under
// <dataDir>/helper/ instead of a socket:
//
//   req.json   {"id":<int>,"op":"list"}  or
//              {"id":<int>,"op":"copy","paths":["<absolute capture path>",...]}
//   resp.json  {"id":<same>,"ok":true,"captures":[{"path","name","time",
//              "bytes","fmt","supported"}],"copied":[{"src","dst"}],"error":""}
//   alive      unix time, rewritten every 2 s; older than 6 s = not running
//
// <dataDir> is the data folder of the built identity (identities.json): the
// payload compiles it in from platform/ps5/helper/Makefile and the title from
// build-title.mk, so neither side names a console path here and two installs
// have two mailboxes. Everything here is pure host/payload code (std::filesystem
// + nlohmann json, no sce calls), so the payload, the title client and the host
// tests share one implementation. The response is written to `resp.tmp` and
// renamed.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/path.h"

namespace splat {

inline constexpr const char* kDefaultCaptureRoot = "/user/av_contents";
inline constexpr const char* kHelperRequestName = "req.json";
inline constexpr const char* kHelperResponseName = "resp.json";
inline constexpr const char* kHelperAliveName = "alive";
inline constexpr const char* kHelperRootName = "root.txt";
inline constexpr std::size_t kHelperMaxCaptures = 300;
inline constexpr int kHelperMaxScanDepth = 5;

// One capture file found under <root>/photo.
struct CaptureInfo {
    Path path;              // absolute path as the helper sees it
    std::string name;       // file name, e.g. 20261025_052805_00338445.jxr
    std::string time;       // YYYYMMDD_HHMMSS, from the name or the mtime
    std::uint64_t bytes = 0;
    std::string fmt;        // "jpg" | "png" | "jxr"
    bool supported = false; // JPG/PNG only; HDR .jxr is listed but not copied
};

// One file the helper put into the inbox.
struct CopiedFile {
    Path src;
    Path dst;
};

enum class HelperOp { List, Copy };

struct HelperRequest {
    long long id = 0;
    HelperOp op = HelperOp::List;
    std::vector<Path> paths;
    std::string error;  // empty = valid

    bool valid() const { return error.empty(); }
};

struct HelperResult {
    bool ok = true;
    std::vector<CaptureInfo> captures;
    std::vector<CopiedFile> copied;
    std::string error;
};

// Parse req.json. Never throws: bad JSON, a missing id/op, an unknown op or a
// malformed paths array returns a request with `error` set.
HelperRequest parse_request(const std::string& text);

// Scan <root>/photo up to kHelperMaxScanDepth levels, skipping any directory
// called "thumbnails", keeping JPG/PNG/JXR files, newest first, at most
// kHelperMaxCaptures entries.
std::vector<CaptureInfo> scan_captures(const Path& root);

// Copy the requested files into `inbox` as capture-<time>-<8hex>.<ext>.
// A path outside `root`, a missing file, a non-capture and an unsupported
// (HDR .jxr) capture are refused and recorded in `error`; an existing
// destination is skipped without overwriting it, and the destination is always
// a name this function derives directly in `inbox`.
//
// Paths are resolved, not compared lexically: the source root and each source
// must answer std::filesystem::canonical() (realpath) and the resolved file must
// stay inside the resolved root, and no component below the root may be a
// symlink (symlink_status()). Both work in the helper payload; a PS5 title is
// refused realpath and lstat (W17), so a copy it asked for is refused rather
// than accepted on a lexical comparison that a symlink could carry outside.
HelperResult copy_captures(const Path& root, const Path& inbox, const std::vector<Path>& paths);

// The response body, exactly the keys the protocol documents.
std::string format_response(long long id, const HelperResult& result);

// req.json text -> resp.json text. The payload loop and the tests' fake
// helper both call this.
std::string handle_request(const std::string& text, const Path& root, const Path& inbox);

// <helper_dir>/root.txt when it holds another absolute path, otherwise
// kDefaultCaptureRoot. The test tree uses this instead of the console root.
Path helper_root(const Path& helper_dir);

// "jpg" | "png" | "jxr" for a capture extension (case-insensitive), else "".
std::string capture_format(const Path& path);
bool is_supported_capture(const Path& path);

}  // namespace splat
