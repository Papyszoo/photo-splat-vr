// SPDX-License-Identifier: GPL-3.0-or-later

#include "gallery/photo_turns.h"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "vfs/atomic_write.h"
#include "common/exif.h"
#include "image/decode.h"
#include "image/format.h"
#include "vfs/io.h"

namespace splat {
namespace {

constexpr const char* kTurnsFile = "photo-turns.json";

// The largest photo-turns.json this build will read: one small entry per file
// the user turned, so a bigger file is not one this app wrote. A read that
// fails (this included) is an empty store, the same answer a damaged file gets.
constexpr std::size_t kMaxTurnsFileBytes = 1u << 20;

int normal_quarters(int quarters) { return ((quarters % 4) + 4) % 4; }

// The formats this app can rewrite without touching pixels. A RAW container is
// excluded by content too: a .tif whose IFD0 carries DNGVersion is a DNG, and a
// CR2 says so at offset 8 (core/image/format.cpp's sniff).
bool rewritable_format(const Path& path, const std::vector<uint8_t>& bytes) {
    const ImageFormat extension = image_format_from_extension(path);
    if (extension != ImageFormat::jpeg && extension != ImageFormat::tiff) return false;
    const ImageFormat sniffed = sniff_image_format(bytes.data(), bytes.size());
    return sniffed == extension;
}

}  // namespace

PhotoTurnStore::PhotoTurnStore(Path state_dir) : state_dir_(std::move(state_dir)) {}

std::map<std::string, int> PhotoTurnStore::load_all() const {
    std::map<std::string, int> turns;
    std::string text;
    if (!vfs::read_text_file(state_dir_ / kTurnsFile, &text, kMaxTurnsFileBytes).ok()) return turns;
    try {
        const nlohmann::json root = nlohmann::json::parse(text);
        const auto entries = root.find("turns");
        if (entries == root.end() || !entries->is_object()) return turns;
        for (auto it = entries->begin(); it != entries->end(); ++it) {
            if (!it.value().is_number_integer()) continue;
            const int quarters = normal_quarters(it.value().get<int>());
            if (quarters != 0) turns[it.key()] = quarters;
        }
    } catch (const std::exception&) {
        // A damaged file is empty: the photos stay listed, their saved turns
        // are gone, and the next save rewrites it cleanly.
    }
    return turns;
}

int PhotoTurnStore::load(const Path& file) const {
    const std::map<std::string, int> turns = load_all();
    const auto found = turns.find(vfs::path_key(file).string());
    return found == turns.end() ? 0 : found->second;
}

Status PhotoTurnStore::save(const Path& file, int quarters) const {
    const int wanted = normal_quarters(quarters);
    const Path cache = state_dir_ / kTurnsFile;
    nlohmann::json root = nlohmann::json::object();
    std::string text;
    if (vfs::read_text_file(cache, &text, kMaxTurnsFileBytes).ok()) {
        try {
            const nlohmann::json parsed = nlohmann::json::parse(text);
            if (parsed.is_object()) root = parsed;
        } catch (const std::exception&) {
            // Rewrite it: a broken store holds no turn worth keeping.
        }
    }
    if (!root.contains("turns") || !root["turns"].is_object()) root["turns"] = nlohmann::json::object();
    const std::string key = vfs::path_key(file).string();
    if (wanted == 0) {
        if (root["turns"].find(key) == root["turns"].end()) return Status::success();
        root["turns"].erase(key);
    } else {
        root["turns"][key] = wanted;
    }
    root["version"] = 1;

    const Status made = vfs::make_dirs(state_dir_);
    if (!made.ok()) return made;
    std::string error;
    if (!vfs::write_text_atomically(cache, root.dump(2) + "\n", &error)) {
        return Status::error(error.empty() ? "cannot write " + cache.string() : error);
    }
    return Status::success();
}

Result<PhotoTurnResult> rotate_photo_file(const Path& file, int delta_quarters,
                                          const PhotoTurnStore& store, bool allow_rewrite) {
    const int saved = store.load(file);
    const int delta = normal_quarters(delta_quarters);
    PhotoTurnResult result;

    if (allow_rewrite) {
        const ImageFormat extension = image_format_from_extension(file);
        // A RAW or any other format is never read for a rewrite and never
        // written to. Only the two formats with a tag writer reach further.
        if (extension == ImageFormat::jpeg || extension == ImageFormat::tiff) {
            const Result<std::vector<uint8_t>> bytes =
                vfs::read_file(file, image::DecodeOptions{}.max_bytes);
            if (!bytes.ok()) {
                // Nothing can be rewritten and the saved route is no help
                // either: the conversion could not read the file. Report the
                // read error instead of inventing a turn that will never be
                // applied (a stale listing after the file was deleted).
                return Result<PhotoTurnResult>::failure(bytes.message());
            }
            if (!rewritable_format(file, bytes.value())) {
                result.note = "this file is not a JPEG or a baseline TIFF";
            } else {
                const Result<ExifData> exif = parse_exif(bytes.value().data(), bytes.value().size());
                if (!exif.ok()) {
                    result.note = exif.message();
                } else {
                    const int current = exif.value().orientation.value_or(1);
                    const int orientation = turn_exif_orientation(current, saved + delta);
                    const Result<std::vector<uint8_t>> rewritten =
                        set_exif_orientation(bytes.value(), orientation);
                    if (!rewritten.ok()) {
                        result.note = rewritten.message();
                    } else if (rewritten.value() == bytes.value()) {
                        // The tag already reads what the whole turn asks for
                        // (four presses back to the start): no write, but the
                        // saved turn must not be applied again later.
                        const Status cleared = store.save(file, 0);
                        if (!cleared.ok()) return Result<PhotoTurnResult>::failure(cleared.message());
                        result.route = PhotoTurnResult::Route::rewritten;
                        result.quarters = 0;
                        result.orientation = orientation;
                        result.file_changed = false;
                        return Result<PhotoTurnResult>::success(result);
                    } else {
                        const Status replaced = vfs::replace_file(file, rewritten.value());
                        if (replaced.ok()) {
                            const Status cleared = store.save(file, 0);
                            if (!cleared.ok()) {
                                // A leftover saved turn would be applied on
                                // top of the new tag: put the original bytes
                                // back rather than double-turn the photo.
                                const Status restored = vfs::replace_file(file, bytes.value());
                                return Result<PhotoTurnResult>::failure(
                                    cleared.message() +
                                    (restored.ok() ? "" : "; and the original could not be restored: " +
                                                              restored.message()));
                            }
                            result.route = PhotoTurnResult::Route::rewritten;
                            result.quarters = 0;
                            result.orientation = orientation;
                            result.file_changed = true;
                            return Result<PhotoTurnResult>::success(result);
                        }
                        result.note = replaced.message();
                    }
                }
            }
        }
    } else {
        result.note = "the source of this capture is not written to";
    }

    // The source file is left alone; the turn is kept for the conversion.
    result.route = PhotoTurnResult::Route::saved;
    result.quarters = normal_quarters(saved + delta);
    const Status saved_status = store.save(file, result.quarters);
    if (!saved_status.ok()) return Result<PhotoTurnResult>::failure(saved_status.message());
    return Result<PhotoTurnResult>::success(result);
}

}  // namespace splat
