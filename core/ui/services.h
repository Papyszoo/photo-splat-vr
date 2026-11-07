// The services the panel screens consume. Every screen takes these through
// UiServices (core/ui/screen.h); a null pointer means the feature is not in
// this build, and the screen says so instead of drawing an empty list.
//
// The interfaces are pure and have no platform dependency. The real adapters
// live in core/ui/job_service.* (W12), core/ui/model_service.* (W13) and
// core/ui/capture_service.* (W23); core/ui/fakes.h has deterministic fakes for
// the tests, tools/ui_preview and fake-mode console runs.
//
// Views are plain structs copied out of the service, so a screen never holds a
// lock or a pointer into service state. `state` is a lowercase token shared
// with the core types: jobs use queued/running/waiting/done/failed/cancelled
// (core/jobs/job_queue.h), models use the ModelState names
// (core/model/model_manager.h).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "common/path.h"
#include "common/status.h"

namespace splat {

// One photo to convert: the file and the clockwise quarter turns the picker
// asked for (0 = as the camera stored it). A Path converts implicitly, so a
// caller that rotates nothing still writes enqueue({path}, ...).
struct PhotoPick {
    Path path;
    int rotate_quarters = 0;
    // The visible-folders key of the tab/folder the pick came from
    // ("source:captures", "location:smb0", "folder:/data/Photos", ...). The
    // picker fills it so the queue never has to guess; empty means "derive it
    // from the path", which is what a build without a policy context does and
    // keeps the old meta.json for tests and previews.
    std::string source_folder;

    PhotoPick() = default;
    // Implicit by design: `{Path("a.jpg")}` is a pick with no rotation.
    PhotoPick(Path source) : path(std::move(source)) {}
    PhotoPick(Path source, int quarters) : path(std::move(source)), rotate_quarters(quarters) {}
    PhotoPick(Path source, int quarters, std::string source_key)
        : path(std::move(source)),
          rotate_quarters(quarters),
          source_folder(std::move(source_key)) {}
};

struct JobView {
    std::string id;
    std::string name;     // photo file name, the row's label
    Path photo;           // the source photo (the folder shows it while converting)
    std::string backend;  // "sharp", "da2", ...
    std::string album;    // "" = the loose-scene album
    std::string kind;     // convert / import / rotate
    std::string state;    // queued/running/waiting/done/failed/cancelled
    float fraction = 0.0f;
    std::string stage;    // short progress label for the running job
    std::string message;  // failure/cancel reason, empty otherwise
    // The visible-folders key of the folder the job reads (empty = derive it
    // from `photo`) and the library the finished scene goes into (empty = the
    // local library). The queue screen filters a row by both, so a job whose
    // source or destination is hidden never appears, never toasts and never
    // counts on the conversion chip.
    std::string source_folder;
    Path output_root;
    // The folder the job wrote, once it is done: the queue's View opens it
    // without looking it up again (a scene uploaded to a share is not under the
    // console's local scene root). Empty before the job finishes.
    Path scene_folder;
};

struct ModelView {
    std::string id;      // backend name, e.g. "sharp"
    std::string name;    // display name
    std::string blurb;   // one line under the name
    uint64_t size_bytes = 0;
    std::string licence_name;  // "Apple research", "Apache-2.0", ...
    bool gated = false;        // needs the licence accepted before download
    std::string state;         // ModelState name (license_required..ready/failed)
    float fraction = 0.0f;
    std::string message;
    bool selected = false;  // the row the converter would load
    bool offered = true;    // false: shown disabled ("not offered yet")
    double backend_seconds = 0.0;  // measured conversion time, 0 = unknown
    // Seconds left of a running download, from its real byte rate; negative when
    // unknown (not downloading, or too little of it seen yet).
    double eta_seconds = -1.0;
};

struct CaptureView {
    Path path;
    std::string name;
    int64_t time = 0;  // seconds since the epoch, newest first in list()
    uint64_t bytes = 0;
    bool supported = false;  // false for HDR .jxr: listed, cannot be selected
};

// One configured network location, as the Locations screen shows it. `state`
// is "ready", "mounting" or "offline: <reason>" (core/vfs/location.h owns the
// store; the adapter publishes the mount result).
struct LocationView {
    std::string id;          // "smb0"; empty for a location not saved yet
    std::string name;        // display name; empty = host/share
    std::string protocol;    // "smb" | "nfs"
    std::string host;
    int port = 0;            // 0 = the protocol default
    std::string share;
    std::string path;
    std::string user;
    std::string password;
    std::string domain;
    bool encrypt = false;
    bool photo_source = false;
    bool splat_library = false;
    // NFS only (L4). AUTH_SYS has no user or password, so these three are the
    // whole credential; 65534 (nobody/nogroup) is the default both here and in
    // core/vfs/location.h. uid/gid are the full AUTH_SYS range: an int would
    // turn any id >= 2^31 negative and a later save would write 0 (root).
    int nfs_version = 3;  // 3 or 4
    uint32_t uid = 65534;
    uint32_t gid = 65534;
    std::string state;       // ready / mounting / offline: ...
};

// The Locations screen's service: the store plus the mount state. add()
// assigns an id; update() keeps it; mount() probes the share on a worker and
// tick() publishes the result, so the UI thread never waits on a NAS.
class ILocationService {
public:
    virtual ~ILocationService() = default;

    virtual std::vector<LocationView> list() const = 0;
    virtual std::string status(const std::string& id) const = 0;
    virtual Status add(const LocationView& location, std::string* id = nullptr) = 0;
    virtual Status update(const LocationView& location) = 0;
    virtual Status remove(const std::string& id) = 0;
    virtual Status mount(const std::string& id) = 0;
    virtual void unmount(const std::string& id) = 0;

    // True while a mount probe is on its worker; the host repaints at a slow
    // rate so "mounting" turns into "ready"/"offline" without a key press.
    virtual bool busy() const { return false; }

    virtual void tick(double /*dt*/) {}
};

class IJobService {
public:
    virtual ~IJobService() = default;

    // Queues one conversion per photo into `album` ("" = loose scenes) with
    // `backend`, each with its own clockwise quarter turns. `output_root`, when
    // set, is the library root the scenes go into (a location path such as
    // `smb0:/scenes`: the runner then stages locally and uploads). Returns an
    // error message fit for a toast when nothing was queued.
    virtual Status enqueue(const std::vector<PhotoPick>& photos, const std::string& backend,
                           const std::string& album, const Path& output_root = Path()) = 0;
    // Queues an import of already-converted splat files into `album`. No model
    // and no rotation: the runner reads each file and thins it. `queued`, when
    // given, receives how many files actually joined the queue: a file that is
    // already queued is skipped, so it is not always splats.size(). The default
    // keeps a service that cannot import out of every screen's way (the picker
    // then shows the error in a toast instead of hiding the tab).
    virtual Status enqueue_import(const std::vector<Path>& /*splats*/,
                                  const std::string& /*album*/,
                                  std::size_t* /*queued*/ = nullptr,
                                  const Path& /*output_root*/ = Path(),
                                  const std::string& /*source_folder*/ = {}) {
        return Status::error("importing splat files is not available in this build");
    }
    // Queues a turn of one scene folder by `quarters` clockwise 90-degree
    // steps (3 = one step left). The default keeps a service that cannot turn
    // scenes out of every screen's way.
    virtual Status enqueue_rotate(const Path& /*scene_folder*/, int /*quarters*/,
                                  const std::string& /*album*/ = std::string()) {
        return Status::error("rotating a scene is not available in this build");
    }
    // Newest last, done rows kept for the session.
    virtual std::vector<JobView> snapshot() const = 0;
    virtual Status cancel(const std::string& id) = 0;
    virtual bool busy() const = 0;
    // The scene folder the viewer has open ("" = none). The host updates it as
    // scenes load; the runner refuses to rewrite that folder, because the
    // renderer is holding the version it read. The enqueue side checks the same
    // rule from the shelf's UiServices::open_scene; this one closes the window
    // between queueing a turn and opening the scene.
    virtual void set_open_scene(const Path& /*folder*/) {}

    // Advances timers/progress. UiApp calls it once per update with the frame
    // time; the fakes need it, real services may ignore it.
    virtual void tick(double /*dt*/) {}
};

class IModelService {
public:
    virtual ~IModelService() = default;

    virtual std::vector<ModelView> list() const = 0;
    // The licence text shown on screen before an acceptance. Never bundled.
    virtual std::string licence_text(const std::string& id) = 0;
    virtual Status accept(const std::string& id) = 0;
    virtual Status download(const std::string& id) = 0;
    virtual Status cancel(const std::string& id) = 0;
    virtual Status remove(const std::string& id) = 0;
    // Only a Ready model can be selected; the choice is persisted by the
    // adapter.
    virtual Status select(const std::string& id) = 0;
    virtual std::string selected() const = 0;

    virtual void tick(double /*dt*/) {}
};

class ICaptureService {
public:
    virtual ~ICaptureService() = default;

    // True while the helper payload answers.
    virtual bool available() const = 0;
    // Re-lists captures; may also try to start the helper.
    virtual Status refresh() = 0;
    virtual std::vector<CaptureView> list() const = 0;
    // Copies supported captures into the inbox, where the normal photo scan
    // finds them.
    virtual Status copy_to_inbox(const std::vector<Path>& paths) = 0;
    // A short status line ("idle", "copying 2 of 4", "Starting", ...).
    virtual std::string state() const = 0;

    virtual void tick(double /*dt*/) {}
};

// The system text-entry dialog (the PS5 IME). The panel keyboard asks this
// first; when it is unavailable the on-panel grid is the fallback. The console
// spike that plugs sceImeDialog in is separate (VR9); this interface is the
// hook, and the desktop/fake implementation always says "not available".
class ITextInput {
public:
    virtual ~ITextInput() = default;

    enum class State { idle, pending, accepted, cancelled };

    virtual bool available() const = 0;
    // Opens the dialog with `title` and `initial`. False when it could not.
    virtual Status open(const std::string& title, const std::string& initial) = 0;
    // Polls the dialog. `accepted` fills `text`; `cancelled` sets it empty; any
    // state other than pending means the dialog is gone.
    virtual State poll(std::string* text) = 0;
    // Abandons an open dialog (leaving the screen, a scene switch).
    virtual void close() = 0;
};

}  // namespace splat
