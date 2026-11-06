// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/fakes.h"

#include <algorithm>
#include <utility>

#include "gallery/visibility.h"
#include "ui/job_service.h"

namespace splat {

Status FakeJobService::enqueue(const std::vector<PhotoPick>& photos, const std::string& backend,
                               const std::string& album, const Path& output_root) {
    if (photos.empty()) return Status::error("no photos to convert");
    last_enqueue_ = photos;
    last_output_root_ = output_root;
    for (const PhotoPick& photo : photos) {
        JobView job;
        job.id = "fake-job-" + std::to_string(next_id_++);
        job.name = photo.path.filename().string();
        job.photo = photo.path;
        if (job.name.empty()) job.name = photo.path.string();
        job.backend = backend;
        job.album = album;
        job.kind = "convert";
        job.state = "queued";
        job.stage = "queued";
        job.source_folder = photo.source_folder;
        job.output_root = output_root;
        jobs_.push_back(std::move(job));
    }
    start_next();
    return Status::success();
}

Status FakeJobService::enqueue_import(const std::vector<Path>& splats, const std::string& album,
                                     std::size_t* queued, const Path& output_root,
                                     const std::string& source_folder) {
    if (splats.empty()) return Status::error("no splat files to import");
    last_import_ = splats;
    last_import_album_ = album;
    last_import_source_ = source_folder;
    last_output_root_ = output_root;
    std::size_t added = 0;
    for (const Path& splat : splats) {
        // The real service skips a file that is already queued or running; the
        // fake does the same so a picker test can check the toast's count.
        const bool duplicate =
            std::any_of(jobs_.begin(), jobs_.end(), [&splat](const JobView& job) {
                return job.photo.lexically_normal() == splat.lexically_normal() &&
                       (job.state == "queued" || job.state == "running");
            });
        if (duplicate) continue;
        JobView job;
        job.id = "fake-job-" + std::to_string(next_id_++);
        job.name = splat.filename().string();
        job.photo = splat;
        if (job.name.empty()) job.name = splat.string();
        job.backend = "import";
        job.album = album;
        job.kind = "import";
        job.state = "queued";
        job.stage = "queued";
        job.source_folder = source_folder;
        job.output_root = output_root;
        jobs_.push_back(std::move(job));
        ++added;
    }
    if (queued != nullptr) *queued = added;
    start_next();
    return added == 0 ? Status::error("already queued") : Status::success();
}

Status FakeJobService::enqueue_rotate(const Path& scene_folder, int quarters,
                                      const std::string& album) {
    if (scene_folder.empty()) return Status::error("no scene folder to rotate");
    last_rotate_scene_ = scene_folder;
    last_rotate_quarters_ = ((quarters % 4) + 4) % 4;
    last_rotate_album_ = album;
    const bool duplicate =
        std::any_of(jobs_.begin(), jobs_.end(), [&scene_folder](const JobView& job) {
            return job.kind == "rotate" &&
                   job.photo.lexically_normal() == scene_folder.lexically_normal() &&
                   (job.state == "queued" || job.state == "running");
        });
    if (duplicate) return Status::error("the scene is already being rotated");
    JobView job;
    job.id = "fake-job-" + std::to_string(next_id_++);
    job.name = scene_folder.filename().string();
    job.photo = scene_folder;
    if (job.name.empty()) job.name = scene_folder.string();
    job.backend = "rotate";
    job.album = album;
    job.kind = "rotate";
    job.state = "queued";
    job.stage = "queued";
    jobs_.push_back(std::move(job));
    start_next();
    return Status::success();
}

std::vector<JobView> FakeJobService::snapshot() const {
    // The same rule the real JobService applies (job_visible): the fake must not
    // leak what the console would hide.
    if (!current_visibility()->enabled) return jobs_;
    std::vector<JobView> visible;
    visible.reserve(jobs_.size());
    for (const JobView& job : jobs_) {
        if (job_visible(job)) visible.push_back(job);
    }
    return visible;
}

void FakeJobService::inject_job(JobView job) {
    if (job.id.empty()) job.id = "fake-job-" + std::to_string(next_id_++);
    jobs_.push_back(std::move(job));
}

void FakeJobService::set_job_state(const std::string& id, const std::string& state,
                                   std::string message) {
    for (JobView& job : jobs_) {
        if (job.id != id) continue;
        job.state = state;
        job.stage = state;
        job.message = std::move(message);
        return;
    }
}

void FakeJobService::start_next() {    if (busy()) return;
    for (JobView& job : jobs_) {
        if (job.state == "queued") {
            job.state = "running";
            job.stage = "loading model";
            job.fraction = 0.0f;
            return;
        }
    }
}

Status FakeJobService::cancel(const std::string& id) {
    for (JobView& job : jobs_) {
        if (job.id != id) continue;
        if (job.state != "queued" && job.state != "running") {
            return Status::error("job " + id + " is " + job.state);
        }
        const bool was_running = job.state == "running";
        job.state = "cancelled";
        job.stage = "cancelled";
        job.message = "cancelled";
        if (was_running) start_next();
        return Status::success();
    }
    return Status::error("no such job: " + id);
}

bool FakeJobService::busy() const {
    for (const JobView& job : jobs_) {
        if (job.state == "running") return true;
    }
    return false;
}

void FakeJobService::tick(double dt) {
    if (dt <= 0.0) return;
    for (JobView& job : jobs_) {
        if (job.state != "running") continue;
        job.fraction = std::min(1.0f, job.fraction + static_cast<float>(dt / kSecondsPerJob));
        if (job.fraction < 0.25f) {
            job.stage = "loading model";
        } else if (job.fraction < 0.8f) {
            job.stage = "decoding gaussians";
        } else if (job.fraction < 1.0f) {
            job.stage = "writing scene";
        } else {
            job.stage = "done";
            job.state = "done";
        }
        if (job.state == "done") start_next();
        return;
    }
}

FakeModelService::FakeModelService() {
    ModelView sharp;
    sharp.id = "sharp";
    sharp.name = "SHARP";
    sharp.blurb = "Recommended, best quality";
    sharp.size_bytes = 2809738232ull;
    sharp.licence_name = "Apple research";
    sharp.gated = true;
    sharp.state = "license_required";
    sharp.backend_seconds = 11.8;
    models_.push_back(std::move(sharp));

    ModelView da2;
    da2.id = "da2";
    da2.name = "DA2-Small";
    da2.blurb = "Fast, approximate scene size";
    da2.size_bytes = 99173660ull;
    da2.licence_name = "Apache-2.0";
    da2.gated = false;
    da2.state = "ready";
    da2.backend_seconds = 0.4;
    models_.push_back(std::move(da2));
}

ModelView* FakeModelService::find(const std::string& id) {
    for (ModelView& model : models_) {
        if (model.id == id) return &model;
    }
    return nullptr;
}

std::string FakeModelService::licence_text(const std::string& id) {
    if (find(id) == nullptr) return {};
    // Three short paragraphs: the licence screen reflows single line breaks, so
    // paragraphs (blank lines) are what keeps the pages predictable in tests.
    return "FAKE LICENCE TEXT (" + id +
           ").\n\nThis is a deterministic stand-in for the real licence.\n\n"
           "The real text is fetched at runtime and never stored in the repository.";
}

Status FakeModelService::accept(const std::string& id) {
    ModelView* model = find(id);
    if (model == nullptr) return Status::error("no such model: " + id);
    if (!model->gated) return Status::error(model->name + " needs no licence");
    if (model->state != "license_required") return Status::success();
    model->state = "accepted";
    model->message.clear();
    return Status::success();
}

Status FakeModelService::download(const std::string& id) {
    ModelView* model = find(id);
    if (model == nullptr) return Status::error("no such model: " + id);
    if (model->gated && model->state == "license_required") {
        return Status::error("accept the " + model->licence_name + " licence first");
    }
    if (model->state == "downloading") return Status::success();
    model->state = "downloading";
    model->fraction = 0.0f;
    model->message.clear();
    return Status::success();
}

Status FakeModelService::cancel(const std::string& id) {
    ModelView* model = find(id);
    if (model == nullptr) return Status::error("no such model: " + id);
    if (model->state != "downloading") return Status::error(model->name + " is not downloading");
    model->state = model->gated ? "accepted" : "ready";
    model->fraction = 0.0f;
    model->message = "cancelled";
    return Status::success();
}

Status FakeModelService::remove(const std::string& id) {
    ModelView* model = find(id);
    if (model == nullptr) return Status::error("no such model: " + id);
    if (selected_ == id) selected_.clear();
    model->selected = false;
    model->state = "accepted";
    model->fraction = 0.0f;
    model->message.clear();
    return Status::success();
}

Status FakeModelService::select(const std::string& id) {
    ModelView* model = find(id);
    if (model == nullptr) return Status::error("no such model: " + id);
    if (model->state != "ready") return Status::error(model->name + " is not ready");
    selected_ = id;
    for (ModelView& entry : models_) entry.selected = entry.id == id;
    return Status::success();
}

void FakeModelService::tick(double dt) {
    if (dt <= 0.0) return;
    for (ModelView& model : models_) {
        if (model.state == "accepted") {
            model.state = "downloading";
            model.fraction = 0.0f;
            continue;
        }
        if (model.state != "downloading") continue;
        model.fraction = std::min(1.0f, model.fraction + static_cast<float>(dt / kDownloadSeconds));
        if (model.fraction >= 1.0f) {
            model.state = "ready";
            model.fraction = 1.0f;
            model.message.clear();
        } else {
            model.message = "downloading";
        }
    }
}

FakeCaptureService::FakeCaptureService(Path root) : root_(std::move(root)) {
    const char* const names[4] = {"capture_01.jpg", "capture_02.png", "capture_03.jpg",
                                  "capture_04.jxr"};
    for (int index = 0; index < 4; ++index) {
        CaptureView capture;
        capture.path = root_ / names[index];
        capture.name = names[index];
        capture.time = 1700000000 + index * 60;
        capture.bytes = 2200000ull + static_cast<uint64_t>(index) * 300000ull;
        capture.supported = index != 3;  // the .jxr capture is HDR
        captures_.push_back(std::move(capture));
    }
}

Status FakeCaptureService::copy_to_inbox(const std::vector<Path>& paths) {
    if (paths.empty()) return Status::error("no captures selected");
    std::size_t copied = 0;
    for (const Path& path : paths) {
        const auto found = std::find_if(captures_.begin(), captures_.end(),
                                        [&path](const CaptureView& view) { return view.path == path; });
        if (found == captures_.end()) return Status::error("unknown capture: " + path.string());
        if (!found->supported) {
            return Status::error(found->name + " is HDR, not supported");
        }
        ++copied;
    }
    state_ = "copied " + std::to_string(copied);
    return Status::success();
}

namespace {

splat::LocationView make_row(const std::string& id, const std::string& host,
                             const std::string& share, const std::string& path,
                             const std::string& state, bool photo_source) {
    splat::LocationView view;
    view.id = id;
    view.name = host + "/" + share;
    view.protocol = "smb";
    view.host = host;
    view.share = share;
    view.path = path;
    view.state = state;
    view.photo_source = photo_source;
    view.splat_library = true;
    return view;
}

}  // namespace

FakeLocationService::FakeLocationService() {
    locations_.push_back(make_row("smb0", "nas.local", "Photos", "scenes", "ready", true));
    locations_.push_back(make_row("smb1", "192.168.1.20", "Scenes", "",
                                  "offline: connect timed out", false));
    next_id_ = 2;
}

std::string FakeLocationService::status(const std::string& id) const {
    for (const LocationView& location : locations_) {
        if (location.id == id) return location.state;
    }
    return "";
}

LocationView* FakeLocationService::find(const std::string& id) {
    for (LocationView& location : locations_) {
        if (location.id == id) return &location;
    }
    return nullptr;
}

Status FakeLocationService::add(const LocationView& location, std::string* id) {
    if (location.host.empty() || location.share.empty()) {
        return Status::error("a location needs a host and a share");
    }
    LocationView stored = location;
    stored.id = "smb" + std::to_string(next_id_++);
    stored.state = "mounting";
    locations_.push_back(stored);
    if (id != nullptr) *id = stored.id;
    return Status::success();
}

Status FakeLocationService::update(const LocationView& location) {
    LocationView* existing = find(location.id);
    if (existing == nullptr) return Status::error("no such location: " + location.id);
    const std::string state = existing->state;
    *existing = location;
    existing->state = state;
    return Status::success();
}

Status FakeLocationService::remove(const std::string& id) {
    for (auto it = locations_.begin(); it != locations_.end(); ++it) {
        if (it->id != id) continue;
        locations_.erase(it);
        return Status::success();
    }
    return Status::error("no such location: " + id);
}

Status FakeLocationService::mount(const std::string& id) {
    LocationView* location = find(id);
    if (location == nullptr) return Status::error("no such location: " + id);
    mounted_.push_back(id);
    // The fake's one failure mode: a host that cannot be reached, reported the
    // way the SMB adapter words it.
    location->state = location->host == "unreachable" ? "offline: connect timed out" : "ready";
    return Status::success();
}

void FakeLocationService::unmount(const std::string& id) {
    mounted_.erase(std::remove(mounted_.begin(), mounted_.end(), id), mounted_.end());
}

void FakeLocationService::tick(double /*dt*/) { ++tick_count_; }

}  // namespace splat
