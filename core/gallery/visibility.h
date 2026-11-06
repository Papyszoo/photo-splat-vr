// The "Visible folders" allow-list: one passcode, one list of scene folders
// that a visitor may see.
//
// Stage U1 (user decision 2026-11-06): the passcode covers the scene library
// only. The photo picker is not filtered at all, so a visitor can browse the
// picker and every photo; what the owner hides is where scenes are listed.
// Everything else about visibility is still decided here, so no screen invents
// a rule of its own: the gallery listing, the boot playlist, the scene
// playlist, the job queue, the Locations list and the viewer's open paths call
// this module.
//
// Shapes
// ------
// A row of the tick list is the album, Unsorted, "Other scenes" or a network
// location a scene is listed under. The state file can still hold the photo
// keys of an earlier build (source:<id>, folder:<abs path>): they load, they
// are inert, and they are not a damaged file - there is no row and no photo
// folder is hidden by them any more.
//
//   album:<name>        a top-level album folder under <scenes>
//   unsorted            the loose scenes directly under <scenes>
//   other               scenes with no trustworthy folder
//   location:<id>       one configured SMB/NFS location
//
// The list is top-level only: a tick publishes everything below it. Ticking a
// folder is a trust boundary, and the owner holds a controller, not a file
// manager (DECISIONS.md "Visible folders").
//
// One rule: the placement
// -----------------------
// A scene is visible only when its *placement* - the album (album:/unsorted),
// the "other" group or the network library (location:) it is listed under - is
// ticked. meta.json, meta.source_folder and the scene's origin decide nothing:
// a scene converted from a photo folder that is no longer tickable stays
// visible in a ticked album (stage U1 dropped the old two-half rule). A scene
// whose meta.json cannot be read is still hidden by an unticked placement, and
// "Other scenes" still has to be ticked to reveal it.
//
// Fail closed
// -----------
// With no passcode every scene folder is visible and nothing else in the app
// changes. A state file that exists but cannot be understood is NOT "no
// passcode": it installs a policy that is enabled, damaged and has an empty
// allow-list, so nothing is shown until a PC deletes the file. A typo must
// never reveal the library.
//
// The protection is an app-UI allow-list, not encryption. Anyone with the
// share, FTP or another app can still open the files.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/passcode.h"
#include "common/path.h"
#include "common/status.h"

namespace splat {

// The one wording for a refused hidden scene destination, shared by the job
// service, the queue screen and the convert dialog. It never names the folder:
// a visitor must not learn that a folder exists and is hidden.
inline constexpr const char* kNotVisibleText = "That folder is not visible";

// photo_source and console_folder are only still here so an old state file
// parses; stage U1 has no row for either and no rule reads them.
enum class VisibilityKind { album, unsorted, other, photo_source, console_folder, location };

// One row of the tick list, in its serialised form ("album:Family A"). The
// text() spelling is the key that the state file carries, so the two cannot
// drift.
struct VisibilityKey {
    VisibilityKind kind = VisibilityKind::other;
    std::string id;

    std::string text() const;
    // False for an unknown prefix, an empty id or a path that is not absolute.
    // A caller that gets a failure must treat the key as "other" (fail closed),
    // never as "allowed".
    static Result<VisibilityKey> parse(std::string_view text);

    static VisibilityKey album(std::string name);
    static VisibilityKey unsorted();
    static VisibilityKey other();
    static VisibilityKey photo_source(std::string id);
    static VisibilityKey console_folder(const Path& folder);
    static VisibilityKey location(std::string id);
};

bool operator==(const VisibilityKey& a, const VisibilityKey& b);
bool operator<(const VisibilityKey& a, const VisibilityKey& b);

// The placement row of a listing directory: "unsorted" for the library root
// itself, "album:<name>" for a direct child, and "other" for anything deeper or
// outside it; a network directory is its location. One function so the gallery
// listing, the boot playlist and Visibility::allows_scene_folder() (which asks
// about the scene folder's parent) cannot drift.
VisibilityKey listing_placement(const Path& scenes_root, const Path& directory);

// The paths the rules need. The host fills them from UiPaths once, at boot
// (core/ui/visibility_setup.h); a unit test fills them from its TempDir, and a
// fixture that installs no visibility leaves them empty (has_context() false).
struct VisibilityContext {
    Path scenes_root;
    Path inbox;
    Path mnt = "/mnt";
    Path data = "/data";
    // paths.inbox.parent_path(): the app's own folder. It is never offered as a
    // console_folder row, and a file under it maps to "other" rather than to a
    // tick the owner cannot see.
    Path app_data;
};

struct Visibility {
    bool enabled = false;
    bool damaged = false;
    PasscodeHash passcode;
    std::vector<VisibilityKey> allowed;  // sorted, unique
    VisibilityContext context;

    // No passcode: every folder visible and every key allowed. The default a
    // process has before install_visibility() runs.
    static Visibility all_visible(VisibilityContext context);
    // The damaged state file: enabled, nothing allowed, no usable hash.
    static Visibility fail_closed(VisibilityContext context);
    bool has_context() const;

    bool allows(const VisibilityKey& key) const;
    bool allows_text(const std::string& text) const;
    bool allows_album(const std::string& album) const;  // "" = unsorted
    bool allows_unsorted() const;
    bool allows_location(const std::string& id) const;
    // The placement rule only: `placement` is the album, Unsorted, "other" or
    // the network location the scene is listed under. `folder` is the scene's
    // own folder, kept for the callers that have it; meta.json is not read.
    bool allows_scene(const Path& folder, const VisibilityKey& placement) const;
    bool allows_scene_folder(const Path& folder) const;
    bool allows_scene_path(const Path& scene_file) const;
    // The provenance key of a photo or scene path: still written to
    // meta.json/PhotoPick::source_folder as information, never used to hide
    // anything (see key_for_source()).
    VisibilityKey key_for_source(const Path& path) const;

    bool check_passcode(const std::string& attempt) const;
    // Each of these writes or removes <state_dir>/visible-folders.json. The
    // passcode rule (4..32 characters) is applied here as well as on the
    // screen, so a hand-written caller cannot store a one-character code.
    Status enable(const Path& state_dir, const std::string& passcode);
    Status change_passcode(const Path& state_dir, const std::string& current,
                           const std::string& next);
    Status disable(const Path& state_dir);
    Status save_allowed(const Path& state_dir) const;
};

// <state_dir>/visible-folders.json. Never in locations.json, never on a share.
Path visible_folders_path(const Path& state_dir);

// A missing file is all_visible(context). An unreadable, malformed, newer or
// otherwise unusable file is fail_closed(context) - never an error a caller
// could mistake for "no passcode".
Result<Visibility> load_visibility(const Path& state_dir, VisibilityContext context);

// The one process-wide policy. Thread-safe: the job worker reads it while the
// UI thread replaces it.
std::shared_ptr<const Visibility> current_visibility();
void set_visibility(Visibility state);

// The test hook: installs `state` on construction and restores whatever policy
// was there on destruction. The release installs through install_visibility().
class VisibilityScope {
public:
    explicit VisibilityScope(Visibility state);
    ~VisibilityScope();
    VisibilityScope(const VisibilityScope&) = delete;
    VisibilityScope& operator=(const VisibilityScope&) = delete;

private:
    std::shared_ptr<const Visibility> previous_;
};

}  // namespace splat
