// SPDX-License-Identifier: GPL-3.0-or-later
#include "scene/scene_view.h"

#include <cmath>
#include <cstddef>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "vfs/atomic_write.h"
#include "common/path.h"
#include "scene/scene_folder.h"
#include "vfs/io.h"

namespace splat {
namespace {

constexpr const char* kViewsFile = "views.json";

// The largest views.json this build will read. The cache holds one small entry
// per scene the user scaled, so a bigger file is not one this app wrote; it is
// treated like a damaged one (no cache), not parsed against its will.
constexpr std::size_t kMaxViewsFileBytes = 1u << 20;

// Reads "scale" out of the view object. False when it is absent, not a number
// or not a positive finite scale, which keeps a hand-edited file from moving
// the scene to a size nobody can use.
bool read_scale_field(const nlohmann::json& view, float* out) {
    if (!view.is_object()) return false;
    const auto scale = view.find("scale");
    if (scale == view.end() || !scale->is_number()) return false;
    const float value = scale->get<float>();
    if (!std::isfinite(value) || value <= 0.0f) return false;
    *out = value;
    return true;
}

// Sets (`present`) or removes the "scale" member of "view", leaving every other
// key alone - including a legacy "rotation", which is erased when the view is
// written. The scale 1 is the *absence* of the key, so a scene nobody scaled
// keeps a meta.json byte-identical to the one its writer produced; an emptied
// "view" object goes too. Returns whether the file's content would change at
// all, so a no-op save does not rewrite it.
bool write_view_scale(nlohmann::json& root, float scale, bool scale_present) {
    if (!root.is_object()) root = nlohmann::json::object();
    auto view = root.find("view");
    if (scale_present) {
        if (view == root.end() || !view->is_object()) {
            root["view"] = nlohmann::json::object();
            view = root.find("view");
        }
        bool changed = false;
        const auto existing = view->find("scale");
        changed = existing == view->end() || *existing != scale;
        (*view)["scale"] = scale;
        if (view->erase("rotation") != 0) changed = true;
        return changed;
    }
    if (view == root.end()) return false;
    if (!view->is_object()) {
        root.erase("view");
        return true;
    }
    const bool had_scale = view->erase("scale") != 0;
    const bool had_rotation = view->erase("rotation") != 0;
    if (view->empty()) root.erase("view");
    return had_scale || had_rotation;
}

// Writes a whole file. A local path goes through the one atomic text write
// (vfs/atomic_write.h), which publishes through the VFS like every other
// write, so nothing ever reads a half-written one. A network
// path goes through its adapter, which either has its own atomic replace or
// reports the share's error; either way false means "the folder refused it",
// which is the read-only case the fallback cache exists for.
bool write_text(const Path& path, const std::string& text, std::string* error) {
    if (vfs::is_remote(path)) {
        const Status written =
            vfs::write_file(path, reinterpret_cast<const uint8_t*>(text.data()), text.size());
        if (!written.ok()) {
            if (error != nullptr) *error = written.message();
            return false;
        }
        return true;
    }
    return vfs::write_text_atomically(path, text, error);
}

// The console-local cache (<state_dir>/views.json) with one scene's entry set
// or removed. `present` false removes it, which is what a write the scene's own
// meta.json took does.
bool write_cache_entry(const Path& state_dir, const Path& key, float scale, bool scale_present,
                       bool present) {
    const Path cache_path = state_dir / kViewsFile;
    nlohmann::json cache = nlohmann::json::object();
    std::string text;
    if (vfs::read_text_file(cache_path, &text, kMaxViewsFileBytes).ok()) {
        try {
            const nlohmann::json parsed = nlohmann::json::parse(text);
            if (parsed.is_object()) cache = parsed;
        } catch (const std::exception&) {
            // Rewrite it: a broken cache holds no view worth keeping.
        }
    }
    if (present) {
        nlohmann::json entry = nlohmann::json::object();
        entry["scale"] = scale;
        cache[key.string()] = entry;
    } else {
        if (cache.find(key.string()) == cache.end()) return true;   // nothing to remove
        cache.erase(key.string());
    }
    // The state directory is local, but the L3-A rule is one place for every
    // filesystem call: creating it goes through the VFS like the write does.
    if (!vfs::make_dirs(state_dir).ok()) return false;
    return write_text(cache_path, cache.dump(2) + "\n", nullptr);
}

// True when the cache file has an entry for this scene: a removal is then worth
// a write, and a scene with none needs no cache at all.
bool cached(const Path& state_dir, const Path& key) {
    std::string text;
    if (!vfs::read_text_file(state_dir / kViewsFile, &text, kMaxViewsFileBytes).ok()) return false;
    try {
        const nlohmann::json cache = nlohmann::json::parse(text);
        return cache.is_object() && cache.find(key.string()) != cache.end();
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace

Path scene_view_key(const Path& scene_folder) { return vfs::path_key(scene_folder); }

bool scene_view_has_scale(const SceneView& view) {
    return std::isfinite(view.scale) && view.scale > 0.0f &&
           std::fabs(view.scale - 1.0f) > 1.0e-6f;
}

bool SceneViewStore::persists_scale(const Path& scene_folder) const {
    return scene_is_imported(scene_folder);
}

bool scene_is_imported(const Path& scene_folder) {
    std::string text;
    if (!vfs::read_text_file(scene_files(scene_folder).meta, &text).ok()) return false;
    // The one imported-scene rule (scene/scene_folder.h), the same one
    // read_scene_meta() reports as origin_present: a meta.json that cannot be
    // parsed is not an import, and save() would refuse it anyway.
    return scene_meta_json_is_imported(text);
}

SceneViewStore::SceneViewStore(Path state_dir) : state_dir_(std::move(state_dir)) {}

Result<SceneView> SceneViewStore::load(const Path& scene_folder, bool* imported_out) const {
    SceneView view;
    const Path meta = scene_files(scene_folder).meta;
    std::string text;
    if (imported_out != nullptr) *imported_out = false;

    // The scene's own meta.json first: it says whether the scene is an import
    // (an "origin" object) and whether it can be read at all. A file that is
    // absent means "no stored view", but one that is there and cannot be read -
    // permissions, a share that dropped - is an error.
    bool imported = false;
    if (vfs::read_text_file(meta, &text).ok()) {
        try {
            const nlohmann::json root = nlohmann::json::parse(text);
            if (!root.is_object()) {
                return Result<SceneView>::failure("meta.json root is not an object: " +
                                                  meta.string());
            }
            imported = scene_meta_json_is_imported(text);
            const auto stored = root.find("view");
            if (stored != root.end() && stored->is_object()) {
                // A legacy "rotation" is ignored here: the viewer has no
                // rotation to apply (DECISIONS.md, 2026-11-05).
                (void)read_scale_field(*stored, &view.scale);
            }
        } catch (const std::exception& error) {
            return Result<SceneView>::failure(std::string("invalid meta.json in ") +
                                              meta.string() + ": " + error.what());
        }
    } else {
        vfs::FileStat inspected;
        const Status looked = vfs::stat(meta, &inspected);
        if (!looked.ok()) {
            return Result<SceneView>::failure("cannot read " + meta.string() + ": " +
                                              looked.message());
        }
        if (inspected.exists) {
            return Result<SceneView>::failure("cannot read " + meta.string());
        }
    }
    if (imported_out != nullptr) *imported_out = imported;

    // The console-local fallback: an entry only exists while the scene's own
    // folder refused the last write, so it is the newer of the two.
    const Path cache_path = state_dir_ / kViewsFile;
    if (vfs::read_text_file(cache_path, &text, kMaxViewsFileBytes).ok()) {
        try {
            const nlohmann::json cache = nlohmann::json::parse(text);
            const auto entry = cache.find(scene_view_key(scene_folder).string());
            if (entry != cache.end() && entry->is_object()) {
                SceneView cached_view;
                if (read_scale_field(*entry, &cached_view.scale)) view = cached_view;
            }
        } catch (const std::exception&) {
            // A corrupt cache is not the scene's problem: it reads from the
            // scene folder, and the next accepted save rewrites the file.
        }
    }
    return Result<SceneView>::success(view);
}

Status SceneViewStore::save(const Path& scene_folder, const SceneView& view,
                            bool* local_only) const {
    if (local_only != nullptr) *local_only = false;
    const Path key = scene_view_key(scene_folder);

    // ---- the scene's own meta.json -----------------------------------------
    const Path meta = scene_files(scene_folder).meta;
    std::string text;
    if (!vfs::read_text_file(meta, &text).ok()) {
        // The scene's own file is not there (or not readable): this is not the
        // read-only-share case the cache exists for.
        return Status::error("cannot read " + meta.string());
    }
    std::string meta_error;
    bool imported = false;
    bool scale_present = false;
    float scale = 1.0f;
    try {
        nlohmann::json root = nlohmann::json::parse(text);
        imported = scene_meta_json_is_imported(text);
        if (imported) {
            scale_present = scene_view_has_scale(view);
            if (scale_present) scale = view.scale;
        } else {
            // A converted scene has no scale of its own; a legacy "scale" is
            // kept untouched rather than replaced by this session's.
            const auto stored = root.find("view");
            scale_present =
                stored != root.end() && stored->is_object() && read_scale_field(*stored, &scale);
        }
        const bool changed = write_view_scale(root, scale, scale_present);
        if (!changed) {
            // The file already says what the caller asked for (and a legacy
            // rotation was not present): it is not rewritten. A cache entry
            // from an earlier read-only session would shadow it, so it goes.
            if (cached(state_dir_, key)) {
                write_cache_entry(state_dir_, key, scale, scale_present, false);
            }
            return Status::success();
        }
        std::string write_error;
        if (write_text(meta, root.dump(2) + "\n", &write_error)) {
            if (cached(state_dir_, key)) {
                write_cache_entry(state_dir_, key, scale, scale_present, false);
            }
            return Status::success();
        }
        meta_error = write_error.empty() ? "cannot write " + meta.string() : write_error;
    } catch (const std::exception& error) {
        return Status::error(std::string("invalid meta.json in ") + meta.string() + ": " +
                             error.what());
    }

    // ---- <state_dir>/views.json --------------------------------------------
    // The scene lives where this console cannot write (a read-only share): the
    // scale is kept here, keyed by the location id and path. A scale of 1 is
    // kept as an entry too: the share still holds the older "scale" it refused
    // to replace, and removing the entry would read that older value back.
    if (!write_cache_entry(state_dir_, key, scale, scale_present, true)) {
        return Status::error(meta_error + "; cannot write " + (state_dir_ / kViewsFile).string());
    }
    if (local_only != nullptr) *local_only = true;
    return Status::success();
}

}  // namespace splat
