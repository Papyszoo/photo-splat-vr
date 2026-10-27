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
    Status enqueue(const std::vector<Path>& photos, const std::string& backend,
                   const std::string& album) override;
    std::vector<JobView> snapshot() const override { return jobs_; }
    Status cancel(const std::string& id) override;
    bool busy() const override;
    void tick(double dt) override;

    // Seconds one fake job takes.
    static constexpr double kSecondsPerJob = 4.0;

private:
    void start_next();

    std::vector<JobView> jobs_;
    int next_id_ = 1;
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
