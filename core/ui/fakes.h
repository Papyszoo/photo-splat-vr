// Deterministic in-memory services for the tests, tools/ui_preview and the
// console's fake mode (UI_FAKE=1). No filesystem, no threads, no clock: progress
// only moves on tick(dt), so a test drives it frame by frame.
//
// FakeJobService: one running job at a time, 4 s per job, queued jobs start in
// order. FakeModelService: SHARP starts licence-gated, DA2-Small starts ready;
// an accepted SHARP walks accepted -> downloading -> ready over 3 s. A gated
// model cannot be downloaded before accept(). FakeCaptureService: four
// captures, the last one HDR (.jxr) and unsupported.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "ui/services.h"
namespace splat {

class FakeJobService : public IJobService {
public:
    Status enqueue(const std::vector<PhotoPick>& photos, const std::string& backend,
                   const std::string& album, const Path& output_root = Path()) override;
    Status enqueue_import(const std::vector<Path>& splats, const std::string& album,
                          std::size_t* queued = nullptr, const Path& output_root = Path(),
                          const std::string& source_folder = {}) override;
    Status enqueue_rotate(const Path& scene_folder, int quarters,
                          const std::string& album = std::string()) override;
    // The same visible-folders filter the real service applies: a hidden-source
    // or hidden-destination row never reaches the Queue screen, the conversion
    // chip or a toast.
    std::vector<JobView> snapshot() const override;
    Status cancel(const std::string& id) override;
    bool busy() const override;
    void tick(double dt) override;

    // Every row, hidden ones included: what publish() would keep in the real
    // service, so a test can prove the filter is a copy and not a deletion.
    const std::vector<JobView>& all_jobs() const { return jobs_; }
    // Test-only: puts one row in the queue as if a job had produced it (a hidden
    // failed job, a job writing to a hidden location), and moves an existing
    // row's state (a queued job that fails while the policy changes).
    void inject_job(JobView job);
    void set_job_state(const std::string& id, const std::string& state,
                       std::string message = std::string());

    const std::vector<PhotoPick>& last_enqueue() const { return last_enqueue_; }
    // What the last enqueue_import() received, so a test can check the picker
    // sent the cell's path and its album without a real queue.
    const std::vector<Path>& last_import() const { return last_import_; }
    const std::string& last_import_album() const { return last_import_album_; }
    // The source key the last enqueue_import() carried (empty = derive it).
    const std::string& last_import_source() const { return last_import_source_; }
    // The destination root the last enqueue carried (empty = the local library).
    const Path& last_output_root() const { return last_output_root_; }
    // The scene folder, turn and album the last enqueue_rotate() received.
    const Path& last_rotate_scene() const { return last_rotate_scene_; }
    int last_rotate_quarters() const { return last_rotate_quarters_; }
    const std::string& last_rotate_album() const { return last_rotate_album_; }

    // Seconds one fake job takes.
    static constexpr double kSecondsPerJob = 4.0;

private:
    void start_next();

    std::vector<JobView> jobs_;
    int next_id_ = 1;
    // The picks the last enqueue received, so a test can see the quarter turns
    // the picker asked for without a real queue.
    std::vector<PhotoPick> last_enqueue_;
    std::vector<Path> last_import_;
    std::string last_import_album_;
    std::string last_import_source_;
    Path last_output_root_;
    Path last_rotate_scene_;
    int last_rotate_quarters_ = 0;
    std::string last_rotate_album_;
};

class FakeModelService : public IModelService {
public:
    FakeModelService();

    std::vector<ModelView> list() const override { return models_; }
    std::string licence_text(const std::string& id) override;
    Status accept(const std::string& id) override;
    Status download(const std::string& id) override;
    Status cancel(const std::string& id) override;
    Status remove(const std::string& id) override;
    Status select(const std::string& id) override;
    std::string selected() const override { return selected_; }
    void tick(double dt) override;

    static constexpr double kDownloadSeconds = 3.0;

private:
    ModelView* find(const std::string& id);

    std::vector<ModelView> models_;
    std::string selected_;
};

class FakeCaptureService : public ICaptureService {
public:
    explicit FakeCaptureService(Path root = Path("captures"));

    bool available() const override { return true; }
    Status refresh() override { return Status::success(); }
    std::vector<CaptureView> list() const override { return captures_; }
    Status copy_to_inbox(const std::vector<Path>& paths) override;
    std::string state() const override { return state_; }

private:
    Path root_;
    std::vector<CaptureView> captures_;
    std::string state_ = "idle";
};

// The Locations screen's fake: two shares that mount "ready" and one that is
// offline, so a test and a fake-mode console run can see every row state
// without a NAS. add() assigns smb0/smb1/... and fails when `host` is
// "unreachable".
class FakeLocationService : public ILocationService {
public:
    FakeLocationService();

    std::vector<LocationView> list() const override {
        // Names are derived like the real service's view_of(): the store keeps
        // an empty name and the display name is host/share.
        std::vector<LocationView> views = locations_;
        for (LocationView& view : views) {
            if (!view.name.empty()) continue;
            view.name = view.share.empty() ? view.host : view.host + "/" + view.share;
        }
        return views;
    }
    std::string status(const std::string& id) const override;
    Status add(const LocationView& location, std::string* id = nullptr) override;
    Status update(const LocationView& location) override;
    Status remove(const std::string& id) override;
    Status mount(const std::string& id) override;
    void unmount(const std::string& id) override;
    bool busy() const override {
        for (const auto& location : locations_) {
            if (location.state == "mounting") return true;
        }
        return false;
    }
    void tick(double dt) override;

    // Counters a test reads.
    const std::vector<std::string>& mounted() const { return mounted_; }
    int tick_count() const { return tick_count_; }
    // The last on_text value the keyboard delivered is set by the caller's
    // lambda; nothing here.

private:
    LocationView* find(const std::string& id);
    std::vector<LocationView> locations_;
    std::vector<std::string> mounted_;
    int next_id_ = 0;
    int tick_count_ = 0;
};

// The desktop and fake-mode text input: always "not available", so the panel
// keyboard uses its own grid. A console job replaces this with an sceImeDialog
// adapter through UiServices::text_input (VR9's hook).
class FakeTextInput : public ITextInput {
public:
    bool available() const override { return false; }
    Status open(const std::string& /*title*/, const std::string& /*initial*/) override {
        return Status::error("the system keyboard is not available in this build");
    }
    State poll(std::string* /*text*/) override { return State::idle; }
    void close() override {}
};

}  // namespace splat
