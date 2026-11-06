// SPDX-License-Identifier: GPL-3.0-or-later
#include "gallery/visibility.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "vfs/atomic_write.h"
#include "vfs/io.h"

namespace splat {
namespace {

constexpr int kFileFormatVersion = 1;
// The state file is the owner's, not the share's: 0600 so another account on
// the console could not read the salt and hash. It is not a secret in the
// "encryption" sense - a PC with file access can read or delete it, which is
// the documented recovery path.
constexpr int kStateFileMode = 0600;
// The policy file is a few hundred bytes. A bigger one is not a file this title
// wrote, and it is never read whole: load_visibility() answers fail closed
// instead (the same answer as an unreadable file).
constexpr size_t kMaxVisibilityFileBytes = 256u << 10;

Path normalize(const Path& path) {
    Path out;
    if (normal_absolute(path, out)) return out;
    return path.lexically_normal();
}

// The components of `path` below `base`, or an empty list when it is not below
// it. Purely lexical, exactly like scene_location(): a PS5 title is refused
// realpath(), so canonical() must never appear here.
std::vector<std::string> relative_parts(const Path& base, const Path& path) {
    std::vector<std::string> parts;
    if (base.empty() || path.empty()) return parts;
    const Path normal_base = normalize(base);
    const Path normal_path = normalize(path);
    const Path relative = normal_path.lexically_relative(normal_base);
    const std::string text = relative.generic_string();
    if (text.empty()) return parts;
    for (const Path& part : relative) {
        const std::string name = part.string();
        if (name.empty() || name == ".") continue;
        if (name == "..") return {};  // not below `base`
        parts.push_back(name);
    }
    return parts;
}

// "usb0" / "ext12" with nothing else numeric in the name.
bool numbered_name(const std::string& name, const std::string& prefix) {
    if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) return false;
    for (size_t index = prefix.size(); index < name.size(); ++index) {
        if (!std::isdigit(static_cast<unsigned char>(name[index]))) return false;
    }
    return true;
}

// Sorted and unique by text(): the state file and every comparison use one
// order, so two equal lists are byte-identical files.
void sort_unique(std::vector<VisibilityKey>* keys) {
    std::sort(keys->begin(), keys->end());
    keys->erase(std::unique(keys->begin(), keys->end()), keys->end());
}

Status write_state(const Path& state_dir, const Visibility& state) {
    nlohmann::json root;
    root["format_version"] = kFileFormatVersion;
    root["passcode"] = {{"algorithm", state.passcode.algorithm},
                        {"iterations", state.passcode.iterations},
                        {"salt", state.passcode.salt},
                        {"hash", state.passcode.hash}};
    root["visible"] = nlohmann::json::array();
    for (const VisibilityKey& key : state.allowed) root["visible"].push_back(key.text());
    const std::string text = root.dump(2) + "\n";

    const Path file = visible_folders_path(state_dir);
    if (!state_dir.empty()) {
        // A missing state directory is not an error worth reporting twice: the
        // write below names the file when it still cannot happen.
        (void)vfs::make_dirs(state_dir);
    }
    std::string error;
    if (!vfs::write_text_atomically(file, text, &error, kStateFileMode)) {
        return Status::error(error.empty() ? "cannot write " + file.string() : error);
    }
    return Status::success();
}

// One parse of the state file. `damaged` is the fail-closed answer: the caller
// installs it instead of showing everything.
Result<Visibility> parse_state(const std::string& text, VisibilityContext context) {
    Visibility state = Visibility::fail_closed(std::move(context));
    try {
        const nlohmann::json root = nlohmann::json::parse(text);
        if (!root.is_object()) return Result<Visibility>::success(std::move(state));
        const auto version = root.find("format_version");
        if (version == root.end() || !version->is_number_integer() ||
            version->get<int>() != kFileFormatVersion) {
            return Result<Visibility>::success(std::move(state));
        }
        const auto stored = root.find("passcode");
        if (stored == root.end() || !stored->is_object()) {
            return Result<Visibility>::success(std::move(state));
        }
        PasscodeHash passcode;
        passcode.algorithm = stored->value("algorithm", std::string{});
        const auto iterations = stored->find("iterations");
        if (iterations == stored->end() || !iterations->is_number_integer()) {
            return Result<Visibility>::success(std::move(state));
        }
        passcode.iterations = iterations->get<int>();
        passcode.salt = stored->value("salt", std::string{});
        passcode.hash = stored->value("hash", std::string{});
        // unusable() is the one check of algorithm, iteration range and the
        // salt/hash lengths: a hand-edited file cannot weaken the hash here.
        if (!passcode.usable()) return Result<Visibility>::success(std::move(state));

        const auto visible = root.find("visible");
        if (visible == root.end() || !visible->is_array()) {
            return Result<Visibility>::success(std::move(state));
        }
        std::vector<VisibilityKey> allowed;
        allowed.reserve(visible->size());
        for (const nlohmann::json& entry : *visible) {
            if (!entry.is_string()) return Result<Visibility>::success(std::move(state));
            const Result<VisibilityKey> key = VisibilityKey::parse(entry.get<std::string>());
            // An unparseable key is fail closed: dropping it silently would
            // show the rest of the library as if the file were intact.
            if (!key.ok()) return Result<Visibility>::success(std::move(state));
            allowed.push_back(key.value());
        }
        sort_unique(&allowed);

        Visibility loaded;
        loaded.enabled = true;
        loaded.damaged = false;
        loaded.passcode = std::move(passcode);
        loaded.allowed = std::move(allowed);
        loaded.context = state.context;
        return Result<Visibility>::success(std::move(loaded));
    } catch (const std::exception&) {
        return Result<Visibility>::success(std::move(state));
    }
}

std::mutex& visibility_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::shared_ptr<const Visibility>& visibility_slot() {
    static std::shared_ptr<const Visibility> slot;
    return slot;
}

}  // namespace

std::string VisibilityKey::text() const {
    switch (kind) {
        case VisibilityKind::album: return "album:" + id;
        case VisibilityKind::unsorted: return "unsorted";
        case VisibilityKind::other: return "other";
        case VisibilityKind::photo_source: return "source:" + id;
        case VisibilityKind::console_folder: return "folder:" + id;
        case VisibilityKind::location: return "location:" + id;
    }
    return "other";
}

Result<VisibilityKey> VisibilityKey::parse(std::string_view text) {
    const auto after = [&](std::string_view prefix) -> std::string {
        return std::string(text.substr(prefix.size()));
    };
    if (text == "unsorted") return Result<VisibilityKey>::success(unsorted());
    if (text == "other") return Result<VisibilityKey>::success(other());
    if (text.compare(0, 6, "album:") == 0 && text.size() > 6) {
        return Result<VisibilityKey>::success(album(after("album:")));
    }
    if (text.compare(0, 7, "source:") == 0 && text.size() > 7) {
        return Result<VisibilityKey>::success(photo_source(after("source:")));
    }
    if (text.compare(0, 7, "folder:") == 0 && text.size() > 7) {
        const Path folder{after("folder:")};
        // A row is an absolute top-level folder; a relative one could mean a
        // different folder under a different working directory.
        if (!folder.is_absolute()) {
            return Result<VisibilityKey>::failure("visibility folder key is not absolute: " +
                                                  std::string(text));
        }
        return Result<VisibilityKey>::success(console_folder(folder));
    }
    if (text.compare(0, 9, "location:") == 0 && text.size() > 9) {
        return Result<VisibilityKey>::success(location(after("location:")));
    }
    return Result<VisibilityKey>::failure("unknown visibility key: " + std::string(text));
}

VisibilityKey VisibilityKey::album(std::string name) {
    return VisibilityKey{VisibilityKind::album, std::move(name)};
}

VisibilityKey VisibilityKey::unsorted() {
    return VisibilityKey{VisibilityKind::unsorted, std::string()};
}

VisibilityKey VisibilityKey::other() { return VisibilityKey{VisibilityKind::other, std::string()}; }

VisibilityKey VisibilityKey::photo_source(std::string id) {
    return VisibilityKey{VisibilityKind::photo_source, std::move(id)};
}

VisibilityKey VisibilityKey::console_folder(const Path& folder) {
    return VisibilityKey{VisibilityKind::console_folder, normalize(folder).generic_string()};
}

VisibilityKey VisibilityKey::location(std::string id) {
    return VisibilityKey{VisibilityKind::location, std::move(id)};
}

bool operator==(const VisibilityKey& a, const VisibilityKey& b) {
    return a.kind == b.kind && a.id == b.id;
}

bool operator<(const VisibilityKey& a, const VisibilityKey& b) { return a.text() < b.text(); }

VisibilityKey listing_placement(const Path& scenes_root, const Path& directory) {
    if (vfs::is_remote(directory)) return VisibilityKey::location(vfs::location_id(directory));
    if (directory.empty()) return VisibilityKey::other();
    // The root itself is the loose scenes; a direct child is an album; anything
    // deeper has no row the owner can tick.
    if (!scenes_root.empty() && normalize(directory) == normalize(scenes_root)) {
        return VisibilityKey::unsorted();
    }
    const std::vector<std::string> parts = relative_parts(scenes_root, directory);
    if (parts.size() == 1) return VisibilityKey::album(parts.front());
    return VisibilityKey::other();
}

Visibility Visibility::all_visible(VisibilityContext context) {
    Visibility state;
    state.enabled = false;
    state.damaged = false;
    state.context = std::move(context);
    return state;
}

Visibility Visibility::fail_closed(VisibilityContext context) {
    Visibility state;
    state.enabled = true;
    state.damaged = true;
    state.allowed.clear();
    state.context = std::move(context);
    return state;
}

bool Visibility::has_context() const { return !context.scenes_root.empty(); }

bool Visibility::allows(const VisibilityKey& key) const {
    if (!enabled) return true;
    return std::binary_search(allowed.begin(), allowed.end(), key);
}

bool Visibility::allows_text(const std::string& text) const {
    const Result<VisibilityKey> key = VisibilityKey::parse(text);
    // A key this build cannot read is not an allowed one.
    return key.ok() && allows(key.value());
}

bool Visibility::allows_album(const std::string& album) const {
    return allows(album.empty() ? VisibilityKey::unsorted() : VisibilityKey::album(album));
}

bool Visibility::allows_unsorted() const { return allows(VisibilityKey::unsorted()); }

bool Visibility::allows_location(const std::string& id) const {
    return allows(VisibilityKey::location(id));
}

bool Visibility::allows_scene(const Path& folder, const VisibilityKey& placement) const {
    if (!enabled) return true;
    // Placement only (stage U1): the album, Unsorted, "other" or the location
    // the scene is listed under. meta.source_folder and the origin are inert.
    (void)folder;
    return allows(placement);
}

bool Visibility::allows_scene_folder(const Path& folder) const {
    if (!enabled) return true;
    // A scene folder is listed under its parent: the root is Unsorted, one
    // album down is that album, and a network library is its location.
    return allows_scene(folder, listing_placement(context.scenes_root, folder.parent_path()));
}

bool Visibility::allows_scene_path(const Path& scene_file) const {
    return allows_scene_folder(scene_file.parent_path());
}

VisibilityKey Visibility::key_for_source(const Path& path) const {
    // Information only (stage U1): the picker still stamps the folder a photo
    // came from into meta.json, and nothing reads it back to hide a photo or a
    // scene any more. The mapping stays so the written key means the same
    // thing it did in stage H.
    if (vfs::is_remote(path)) return VisibilityKey::location(vfs::location_id(path));

    // The inbox first: it is the app's own upload folder, and it sits under the
    // app data folder, which is under the data root.
    if (!relative_parts(context.inbox, path).empty()) return VisibilityKey::photo_source("inbox");

    // A USB stick is one source; an extN entry is a console storage root whose
    // top-level folders are the tickable rows.
    const std::vector<std::string> mnt_parts = relative_parts(context.mnt, path);
    if (!mnt_parts.empty()) {
        if (numbered_name(mnt_parts.front(), "usb")) {
            return VisibilityKey::photo_source(mnt_parts.front());
        }
        if (numbered_name(mnt_parts.front(), "ext") && mnt_parts.size() >= 2) {
            return VisibilityKey::console_folder(context.mnt / mnt_parts[0] / mnt_parts[1]);
        }
    }

    // Scenes before the data root: the library normally lives under the app's
    // own folder, which is inside /data, and an album key is what the owner
    // ticks for it.
    const std::vector<std::string> scene_parts = relative_parts(context.scenes_root, path);
    if (!scene_parts.empty()) {
        return scene_parts.size() == 1 ? VisibilityKey::unsorted()
                                       : VisibilityKey::album(scene_parts.front());
    }

    const std::vector<std::string> data_parts = relative_parts(context.data, path);
    if (data_parts.size() >= 2) {
        const Path top = context.data / data_parts.front();
        // The app's own folder is never a row: its inbox is a source and its
        // scenes are album rows, so a file that only names it has no tick.
        if (!context.app_data.empty() && normalize(top) == normalize(context.app_data)) {
            return VisibilityKey::other();
        }
        return VisibilityKey::console_folder(top);
    }
    return VisibilityKey::other();
}

bool Visibility::check_passcode(const std::string& attempt) const {
    if (!enabled || damaged) return false;
    return verify_passcode(attempt, passcode);
}

Status Visibility::enable(const Path& state_dir, const std::string& passcode_text) {
    if (passcode_text.size() < kPasscodeMinLength || passcode_text.size() > kPasscodeMaxLength) {
        return Status::error("the passcode must be 4 to 32 characters");
    }
    Visibility next = *this;
    next.enabled = true;
    next.damaged = false;
    next.passcode = make_passcode_hash(passcode_text);
    // The first passcode starts with nothing ticked: fail closed until the owner
    // says what may be seen (DECISIONS.md).
    next.allowed.clear();
    const Status saved = write_state(state_dir, next);
    if (!saved.ok()) return saved;
    *this = std::move(next);
    return Status::success();
}

Status Visibility::change_passcode(const Path& state_dir, const std::string& current,
                                   const std::string& next_text) {
    if (!enabled || damaged) return Status::error("no passcode is set");
    if (!check_passcode(current)) return Status::error("wrong passcode");
    if (next_text.size() < kPasscodeMinLength || next_text.size() > kPasscodeMaxLength) {
        return Status::error("the passcode must be 4 to 32 characters");
    }
    Visibility next = *this;
    // A new salt every time: the same passcode twice must not produce the same
    // file, or two consoles would share a hash.
    next.passcode = make_passcode_hash(next_text);
    const Status saved = write_state(state_dir, next);
    if (!saved.ok()) return saved;
    *this = std::move(next);
    return Status::success();
}

Status Visibility::disable(const Path& state_dir) {
    const Path file = visible_folders_path(state_dir);
    vfs::FileStat status;
    if (vfs::stat(file, &status).ok() && status.exists) {
        const Status removed = vfs::remove_file(file);
        if (!removed.ok()) return Status::error(removed.message());
    }
    enabled = false;
    damaged = false;
    allowed.clear();
    passcode = PasscodeHash{};
    return Status::success();
}

Status Visibility::save_allowed(const Path& state_dir) const {
    if (!enabled) return Status::error("no passcode is set");
    Visibility next = *this;
    sort_unique(&next.allowed);
    return write_state(state_dir, next);
}

Path visible_folders_path(const Path& state_dir) { return state_dir / "visible-folders.json"; }

Result<Visibility> load_visibility(const Path& state_dir, VisibilityContext context) {
    const Path file = visible_folders_path(state_dir);
    vfs::FileStat status;
    if (!vfs::stat(file, &status).ok() || !status.exists) {
        // A stat the adapter reports as "nothing there" is the normal missing
        // case, but the desktop adapter also answers that for a path it was
        // refused (a permission on the folder). The folder's own listing says
        // whether the file is there: if it is, this is fail closed, never "no
        // passcode".
        std::vector<vfs::DirEntry> entries;
        if (vfs::list(state_dir, &entries).ok()) {
            for (const vfs::DirEntry& entry : entries) {
                if (entry.name != "visible-folders.json") continue;
                return Result<Visibility>::success(Visibility::fail_closed(std::move(context)));
            }
        }
        return Result<Visibility>::success(Visibility::all_visible(std::move(context)));
    }
    std::string text;
    // The policy file is a few hundred bytes; the cap keeps a file that grew (or
    // was replaced by something else) from being read whole into memory. Over
    // the cap is the fail-closed case, like an unreadable file.
    if (!vfs::read_text_file(file, &text, kMaxVisibilityFileBytes).ok()) {
        // The file is there and cannot be read: that is not "no passcode".
        return Result<Visibility>::success(Visibility::fail_closed(std::move(context)));
    }
    return parse_state(text, std::move(context));
}

std::shared_ptr<const Visibility> current_visibility() {
    std::lock_guard<std::mutex> lock(visibility_mutex());
    std::shared_ptr<const Visibility>& slot = visibility_slot();
    if (slot == nullptr) {
        // No host installed a policy (a unit test, tools/ui_preview): every
        // folder visible and no context, exactly the pre-stage-H behaviour.
        static const auto fallback =
            std::make_shared<const Visibility>(Visibility::all_visible(VisibilityContext{}));
        return fallback;
    }
    return slot;
}

void set_visibility(Visibility state) {
    sort_unique(&state.allowed);
    std::lock_guard<std::mutex> lock(visibility_mutex());
    visibility_slot() = std::make_shared<const Visibility>(std::move(state));
}

VisibilityScope::VisibilityScope(Visibility state) {
    {
        std::lock_guard<std::mutex> lock(visibility_mutex());
        previous_ = visibility_slot();
    }
    set_visibility(std::move(state));
}

VisibilityScope::~VisibilityScope() {
    std::lock_guard<std::mutex> lock(visibility_mutex());
    visibility_slot() = previous_;
}

}  // namespace splat
