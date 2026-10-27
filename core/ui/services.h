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
// with the core types: jobs use queued/running/done/failed/cancelled
// (core/jobs/job_queue.h), models use the ModelState names
// (core/model/model_manager.h).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/path.h"
#include "common/status.h"

namespace splat {

struct JobView {
    std::string id;
    std::string name;     // photo file name, the row's label
    Path photo;           // the source photo (the folder shows it while converting)
    std::string backend;  // "sharp", "da2", ...
    std::string album;    // "" = the loose-scene album
    std::string state;    // queued/running/done/failed/cancelled
    float fraction = 0.0f;
    std::string stage;    // short progress label for the running job
    std::string message;  // failure/cancel reason, empty otherwise
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
};

struct CaptureView {
    Path path;
    std::string name;
    int64_t time = 0;  // seconds since the epoch, newest first in list()
    uint64_t bytes = 0;
    bool supported = false;  // false for HDR .jxr: listed, cannot be selected
};

class IJobService {
public:
    virtual ~IJobService() = default;

    // Queues one conversion per photo into `album` ("" = loose scenes) with
    // `backend`. Returns an error message fit for a toast when nothing was
    // queued.
    virtual Status enqueue(const std::vector<Path>& photos, const std::string& backend,
                           const std::string& album) = 0;
    // Newest last, done rows kept for the session.
    virtual std::vector<JobView> snapshot() const = 0;
    virtual Status cancel(const std::string& id) = 0;
    virtual bool busy() const = 0;

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
