#include "gallery/samples.h"

#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#include "gallery/fs_util.h"
#include "gallery/gallery.h"

namespace splat {
namespace {

bool path_exists(const Path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
}

// Opens `marker` for writing (creating its parents), which is also the claim
// that this start owns the seed: the file exists from here on, so a later
// failure cannot make the next start copy the bundle again.
bool claim_marker(const Path& marker, std::ofstream& out, std::string* error) {
    const Path parent = marker.parent_path();
    if (!parent.empty()) {
        std::error_code code;
        std::filesystem::create_directories(parent, code);
        if (code) {
            *error = "cannot create " + parent.string() + ": " + code.message();
            return false;
        }
    }
    out.open(marker, std::ios::binary | std::ios::trunc);
    if (!out) {
        *error = "cannot write " + marker.string();
        return false;
    }
    return true;
}

// Gives the claim back so the next start tries the seed again.
void abandon_marker(const Path& marker, std::ofstream& out) {
    out.close();
    std::error_code ignored;
    std::filesystem::remove(marker, ignored);
}

// A plain recursive copy of a scene folder, file by file. std::filesystem::copy
// asks symlink_status() (lstat) first, which a PS5 title is refused (gallery.cpp
// documents the measurement), so the copy uses the entry type the directory
// read already carries and copies regular files. The bundle is our own read-only
// material, so following a link is harmless.
bool copy_folder(const Path& source, const Path& target, std::string* error) {
    std::error_code code;
    std::filesystem::create_directories(target, code);
    if (code) {
        *error = "cannot create " + target.string() + ": " + code.message();
        return false;
    }

    std::filesystem::directory_iterator it(source,
                                          std::filesystem::directory_options::skip_permission_denied,
                                          code);
    const std::filesystem::directory_iterator end;
    while (!code && it != end) {
        std::error_code entry_error;
        const Path from = it->path();
        const Path to = target / from.filename();
        if (it->is_directory(entry_error) && !entry_error) {
            if (!copy_folder(from, to, error)) return false;
        } else {
            entry_error.clear();
            std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing,
                                       entry_error);
            if (entry_error) {
                *error = "cannot copy " + from.string() + ": " + entry_error.message();
                return false;
            }
        }
        it.increment(code);
    }
    if (code) {
        *error = "cannot read " + source.string() + ": " + code.message();
        return false;
    }
    return true;
}

}  // namespace

SamplesSeed seed_samples(const Path& bundle, const Path& root, const Path& marker) {
    SamplesSeed result;

    // One start per install: after the first successful seed the marker is the
    // only state, so deleting the album (or one scene in it) is final.
    //
    // The marker is asked once, with its error code: "cannot examine it" is not
    // "absent". Seeding on a status error would copy the bundle again on every
    // start (the case an unwritable or damaged data directory produces), so it
    // counts as seeded and reports why.
    std::error_code marker_error;
    const bool marked = std::filesystem::exists(marker, marker_error);
    if (marked) {
        result.already_seeded = true;
        return result;
    }
    if (marker_error) {
        result.already_seeded = true;
        result.error = "cannot examine " + marker.string() + ": " + marker_error.message();
        return result;
    }
    // A build without the samples folder in its package seeds nothing and
    // writes no marker; a later package that does carry them still gets its
    // chance.
    if (!is_dir(bundle)) return result;

    // Claim the marker before copying anything: a data directory that cannot
    // take the marker stops the seed here, so every start costs one failed
    // status and no copy. A copy that fails afterwards gives the claim back, so
    // the next start retries; a marker write that fails after a successful copy
    // leaves the claim in place, so the next start does not seed again.
    std::ofstream marker_out;
    if (!claim_marker(marker, marker_out, &result.error)) return result;

    // The album goes through the gallery's own rule, so the reserved and
    // duplicate checks stay in one place.
    const Path album = root / kSamplesAlbumName;
    if (!is_dir(album)) {
        const Status created = create_album(root, kSamplesAlbumName);
        if (!created.ok()) {
            abandon_marker(marker, marker_out);
            result.error = created.message();
            return result;
        }
    }

    // A bundled album is a folder of scene folders, exactly what list_scenes
    // reads at the root of a gallery.
    for (const SceneEntry& scene : list_scenes(bundle)) {
        const Path target = album / scene.name;
        // Same name already there: the user's folder wins, never overwrite.
        if (path_exists(target)) continue;

        std::string copy_error;
        if (!copy_folder(scene.folder, target, &copy_error)) {
            // Drop the partial copy so a retry starts clean, and the claim with
            // it.
            std::error_code ignored;
            std::filesystem::remove_all(target, ignored);
            abandon_marker(marker, marker_out);
            result.error = copy_error;
            return result;
        }
        ++result.scenes;
    }

    marker_out << "samples seeded: " << result.scenes << " scene(s)\n";
    marker_out.flush();
    if (!marker_out) {
        // The claim is still on disk, so this start is the last one that seeds;
        // say the count may be missing, and do not retry.
        result.error = "failed while writing " + marker.string();
    }
    return result;
}

}  // namespace splat
