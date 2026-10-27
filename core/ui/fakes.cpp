// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/fakes.h"

#include <algorithm>
#include <utility>

namespace splat {

Status FakeJobService::enqueue(const std::vector<Path>& photos, const std::string& backend,
                               const std::string& album) {
    if (photos.empty()) return Status::error("no photos to convert");
    for (const Path& photo : photos) {
        JobView job;
        job.id = "fake-job-" + std::to_string(next_id_++);
        job.name = photo.filename().string();
        job.photo = photo;
        if (job.name.empty()) job.name = photo.string();
        job.backend = backend;
        job.album = album;
        job.state = "queued";
        job.stage = "queued";
        jobs_.push_back(std::move(job));
    }
    start_next();
    return Status::success();
}

void FakeJobService::start_next() {
    if (busy()) return;
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

}  // namespace splat
