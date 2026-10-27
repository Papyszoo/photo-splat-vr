// The panel preview registry: every screen of the panel, drawn by the real
// UiApp and screens over the fake services and a throwaway scene/photo tree,
// so the look can be judged on the desktop without a console. "pattern" is
// the canvas diagnostic (test_pattern.cpp), not a shipped screen, and is left
// out of --all.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "patterns.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "common/image.h"
#include "common/path.h"
#include "jobs/image_io.h"
#include "scene/scene_folder.h"
#include "scene/splat_scene.h"
#include "ui/app.h"
#include "ui/fakes.h"
#include "ui/screens/registry.h"
#include "test_pattern.h"
#include "ui/ui_input.h"

namespace splat {
namespace ui_preview {
namespace {

void draw_test_pattern_screen(UiCanvas& canvas) {
    TestPatternContent content;
    content.update(UiInput{});
    content.draw(canvas);
}

// ---------------------------------------------------------------------------
// Throwaway content.

// A small landscape "photo": sky, a sun, two ridges and ground, tinted per
// variant, so tiles look like pictures rather than test cards.
Image landscape(int width, int height, int variant) {
    Image image;
    image.resize(width, height);
    const float hue = static_cast<float>((variant * 47) % 360) * 3.14159265f / 180.0f;
    const float sky_top[3] = {40.0f + 30.0f * std::cos(hue), 70.0f + 30.0f * std::sin(hue), 150.0f};
    const float sky_low[3] = {230.0f, 160.0f + 40.0f * std::sin(hue * 1.7f),
                              110.0f + 50.0f * std::cos(hue)};
    const float sun_x = width * (0.25f + 0.5f * static_cast<float>((variant * 37) % 100) / 100.0f);
    const float sun_y = height * 0.42f;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(width);
            const float t = static_cast<float>(y) / static_cast<float>(height);
            float rgb[3];
            for (int c = 0; c < 3; ++c) rgb[c] = sky_top[c] + (sky_low[c] - sky_top[c]) * (t / 0.6f);
            const float dx = static_cast<float>(x) - sun_x;
            const float dy = static_cast<float>(y) - sun_y;
            const float sun = std::exp(-(dx * dx + dy * dy) / (height * height * 0.004f));
            for (int c = 0; c < 3; ++c) rgb[c] += 120.0f * sun;
            const float ridge_far =
                0.55f + 0.06f * std::sin(fx * 9.0f + variant) + 0.03f * std::sin(fx * 23.0f);
            const float ridge_near = 0.70f + 0.08f * std::sin(fx * 5.0f + variant * 2.0f);
            if (t > ridge_far) {
                rgb[0] = 60.0f + 20.0f * std::sin(hue);
                rgb[1] = 70.0f + 10.0f * std::cos(hue);
                rgb[2] = 100.0f;
            }
            if (t > ridge_near) {
                const float shade = 1.0f - (t - ridge_near) * 1.6f;
                rgb[0] = (30.0f + 30.0f * std::cos(hue)) * shade;
                rgb[1] = (60.0f + 20.0f * std::sin(hue)) * shade;
                rgb[2] = 40.0f * shade;
            }
            uint8_t* pixel = image.pixel(x, y);
            for (int c = 0; c < 3; ++c) {
                pixel[c] = static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, rgb[c])));
            }
        }
    }
    return image;
}

std::vector<uint8_t> landscape_jpeg(int width, int height, int variant) {
    const Result<std::vector<uint8_t>> encoded = encode_jpeg(landscape(width, height, variant), 88);
    if (encoded.ok()) return encoded.value();
    return {0xFF, 0xD8, 0xFF, 0xD9};
}

void write_file(const Path& path, const std::vector<uint8_t>& bytes) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

struct PreviewScene {
    std::string album;  // "" = the loose scenes
    std::string name;
    const char* backend;
    uint32_t splats;
    double seconds;
    bool thumb;
};

void write_preview_scene(const Path& root, const PreviewScene& spec, int variant) {
    const Path folder = spec.album.empty() ? root / spec.name : root / spec.album / spec.name;
    SplatScene scene;
    scene.resize(8);
    for (size_t index = 0; index < 8; ++index) {
        const float step = static_cast<float>(index);
        scene.position[index * 3 + 0] = 0.2f * step - 0.7f;
        scene.position[index * 3 + 1] = 0.1f * step;
        scene.position[index * 3 + 2] = 1.5f;
        scene.rotation[index * 4 + 3] = 1.0f;
        scene.opacity_logit[index] = 2.0f;
    }
    SceneMeta meta;
    meta.backend = spec.backend;
    meta.intrinsics.width = 320;
    meta.intrinsics.height = 240;
    meta.intrinsics.fx = 280.0f;
    meta.intrinsics.fy = 280.0f;
    meta.intrinsics.cx = 160.0f;
    meta.intrinsics.cy = 120.0f;
    meta.seconds_taken = spec.seconds;
    meta.splat_count = spec.splats;

    const std::vector<uint8_t> jpeg = landscape_jpeg(320, 240, variant);
    write_scene_folder(folder, scene, meta, jpeg, jpeg);  // stores the scene's own count
    write_scene_meta(scene_files(folder).meta, meta);     // the count the preview should show
    if (!spec.thumb) {
        std::error_code error;
        std::filesystem::remove(scene_files(folder).thumbnail, error);
    }
}

// What a preview tree holds.
enum Content : unsigned {
    kNothing = 0,
    kLibrary = 1u << 0,   // folders with scenes, some without thumbnails
    kMany = 1u << 1,      // one folder with many scenes
    kLongNames = 1u << 2, // a folder and scenes with long names
    kPhotos = 1u << 3,    // inbox, a USB stick, console folders
};

constexpr const char* kTrip = "Krakow trip";
constexpr const char* kHoliday = "Summer holiday 2026";
constexpr const char* kLong = "Grandparents garden and the old orchard in May";

// A temp data root (scenes, inbox, mnt, data) removed at scope exit.
class PreviewTree {
public:
    explicit PreviewTree(unsigned content) {
        static int counter = 0;
        const std::filesystem::path base = std::filesystem::temp_directory_path();
        for (int attempt = 0; attempt < 100 && root_.empty(); ++attempt) {
            const std::filesystem::path candidate =
                base / ("psplat-ui-preview-" + std::to_string(++counter));
            std::error_code error;
            if (std::filesystem::create_directories(candidate, error)) root_ = candidate;
        }
        std::error_code error;
        std::filesystem::create_directories(scenes(), error);
        std::filesystem::create_directories(inbox(), error);
        std::filesystem::create_directories(root_ / "mnt", error);
        std::filesystem::create_directories(data(), error);
        int variant = 0;
        if ((content & kLibrary) != 0) {
            const PreviewScene specs[] = {
                {"", "balcony-evening", "sharp", 1450000, 12.4, true},
                {"", "hallway", "sharp", 210000, 8.6, false},
                {kTrip, "market-square", "sharp", 1234567, 12.4, true},
                {kTrip, "cathedral-steps", "sharp", 980000, 11.0, true},
                {kTrip, "pizza-close-up", "depth-anything-v2-small", 213000, 0.4, true},
                {kTrip, "river-bank", "sharp", 98000, 6.2, true},
                {kTrip, "old-town-gate", "sharp", 45000, 3.1, false},
                {kTrip, "castle-courtyard", "depth-anything-v2-small", 330000, 9.9, true},
                {kTrip, "tram-stop", "sharp", 640000, 10.8, true},
                {"Garden", "garden-night", "depth-anything-v2-large", 640000, 10.8, true},
                {"Garden", "sofa-test", "sharp", 12000, 2.0, false},
                {"Kitchen tests", "kitchen-12-aug", "sharp", 1234567, 12.4, false},
            };
            for (const PreviewScene& spec : specs) write_preview_scene(scenes(), spec, variant++);
            std::filesystem::create_directories(scenes() / "Empty folder", error);
        }
        if ((content & kMany) != 0) {
            for (int index = 0; index < 23; ++index) {
                write_preview_scene(scenes(),
                                    PreviewScene{kHoliday, "beach-" + std::to_string(100 + index),
                                                 "sharp", 900000u + 1000u * index, 11.5,
                                                 index % 5 != 3},
                                    variant++);
            }
        }
        if ((content & kLongNames) != 0) {
            write_preview_scene(
                scenes(),
                PreviewScene{kLong, "apple-trees-in-blossom-behind-the-wooden-shed-at-sunset",
                             "sharp", 1300000, 12.0, true},
                variant++);
            write_preview_scene(scenes(),
                                PreviewScene{kLong, "grandma-and-the-cat-on-the-porch-steps",
                                             "depth-anything-v2-small", 220000, 0.5, true},
                                variant++);
            write_preview_scene(scenes(), PreviewScene{kLong, "orchard", "sharp", 45000, 3.0, false},
                                variant++);
        }
        if ((content & kPhotos) != 0) {
            const char* inbox_names[] = {"IMG_2041.jpg", "IMG_2042.jpg", "IMG_2047.jpg",
                                         "IMG_2050.jpg", "IMG_2051.jpg", "IMG_2063.jpg",
                                         "IMG_2064.jpg", "IMG_2070.jpg", "IMG_2071.jpg"};
            int index = 0;
            for (const char* name : inbox_names) {
                write_file(inbox() / name, landscape_jpeg(320, 240, 20 + index++));
            }
            write_file(root_ / "mnt" / "usb0" / "DCIM" / "P1000.jpg", landscape_jpeg(320, 240, 40));
            write_file(root_ / "mnt" / "usb0" / "DCIM" / "P1001.jpg", landscape_jpeg(320, 240, 41));
            write_file(data() / "photos" / "2026-10" / "walk-01.jpg", landscape_jpeg(320, 240, 50));
            write_file(data() / "photos" / "2026-10" / "walk-02.jpg", landscape_jpeg(320, 240, 51));
            write_file(data() / "photos" / "2026-10" / "walk-03.png", landscape_jpeg(320, 240, 52));
            std::filesystem::create_directories(data() / "photos" / "2026-10" / "raw", error);
            std::filesystem::create_directories(data() / "photos" / "2026-09", error);
            std::filesystem::create_directories(root_ / "mnt" / "ext0" / "pictures", error);
        }
    }

    ~PreviewTree() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    PreviewTree(const PreviewTree&) = delete;
    PreviewTree& operator=(const PreviewTree&) = delete;

    Path scenes() const { return root_ / "scenes"; }
    // Laid out like the console: the inbox inside the app's folder in /data.
    Path inbox() const { return root_ / "data" / "ps5-splat-vr" / "inbox"; }
    Path data() const { return root_ / "data"; }
    Path mnt() const { return root_ / "mnt"; }
    Path state() const { return root_ / "state"; }

private:
    std::filesystem::path root_;
};

// One running app over a preview tree and the fakes.
struct Preview {
    PreviewTree tree;
    FakeJobService jobs;
    FakeModelService models;
    FakeCaptureService captures;
    UiServices services;
    UiApp app;

    explicit Preview(unsigned content, bool with_captures = false, float theme_scale = 1.0f)
        : tree(content), captures(tree.inbox()),
          app(services_for(*this, with_captures),
              theme_scale == 1.0f ? default_theme()
                                  : scaled_theme(default_theme(), theme_scale)) {
        register_default_screens(app.registry());
        app.start();
    }

    static UiServices& services_for(Preview& self, bool with_captures) {
        self.services.paths.scenes = self.tree.scenes();
        self.services.paths.inbox = self.tree.inbox();
        self.services.paths.mnt = self.tree.mnt();
        self.services.paths.data = self.tree.data();
        self.services.paths.state = self.tree.state();
        self.services.jobs = &self.jobs;
        self.services.models = &self.models;
        self.services.captures = with_captures ? &self.captures : nullptr;
        return self.services;
    }

    // Frames until the asynchronous thumbnails are in (the ThumbCache worker
    // decodes on its own thread) and the canvas has settled. Fake time does
    // not move (dt 0), so a job's progress stays where the preview put it.
    void pump(UiCanvas& canvas) {
        UiInput idle;
        for (int frame = 0; frame < 120; ++frame) {
            app.update(idle);
            app.draw(canvas);
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        app.invalidate();
        app.draw(canvas);
    }

    void tap(uint32_t action, int times = 1) {
        for (int index = 0; index < times; ++index) {
            UiInput input;
            input.pressed = action;
            app.update(input);
        }
    }

    void open_folder(const std::string& name) {
        ScreenArgs args;
        args.a = name;
        app.nav().push(ScreenId::Scenes, args);
    }

    // SHARP downloaded and remembered, DA2-Small ready: both offered.
    void install_models() {
        models.accept("sharp");
        models.download("sharp");
        models.tick(FakeModelService::kDownloadSeconds * 2.0);
        models.select("sharp");
    }
};

// ---------------------------------------------------------------------------
// The screens.

void draw_home(UiCanvas& canvas) {
    Preview preview(kLibrary | kLongNames);
    preview.tap(Right);  // focus the first named folder
    preview.pump(canvas);
}

void draw_home_empty(UiCanvas& canvas) {
    Preview preview(kNothing);
    preview.pump(canvas);
}

void draw_home_menu(UiCanvas& canvas) {
    Preview preview(kLibrary);
    preview.tap(Right);  // "Empty folder": Rename and Delete
    preview.tap(Context);
    preview.pump(canvas);
}

void draw_home_converting(UiCanvas& canvas) {
    Preview preview(kLibrary | kPhotos);
    preview.jobs.enqueue({preview.tree.inbox() / "IMG_2041.jpg", preview.tree.inbox() / "IMG_2047.jpg"},
                         "sharp", kTrip);
    preview.jobs.tick(1.6);
    preview.tap(Down);
    preview.tap(Right);  // the "New folder" tile at the end
    preview.pump(canvas);
}

void draw_folder(UiCanvas& canvas) {
    Preview preview(kLibrary);
    preview.open_folder(kTrip);
    preview.tap(Right);  // second scene
    preview.pump(canvas);
}

void draw_folder_empty(UiCanvas& canvas) {
    Preview preview(kLibrary);
    preview.open_folder("Empty folder");
    preview.pump(canvas);
}

void draw_folder_many(UiCanvas& canvas) {
    Preview preview(kMany);
    preview.open_folder(kHoliday);
    preview.tap(Down, 3);  // scrolled: the chevrons show there is more
    preview.tap(Right);
    preview.pump(canvas);
}

void draw_folder_menu(UiCanvas& canvas) {
    Preview preview(kLibrary);
    preview.open_folder(kTrip);
    preview.tap(Context);
    preview.pump(canvas);
}

void draw_folder_delete(UiCanvas& canvas) {
    Preview preview(kLibrary);
    preview.open_folder(kTrip);
    preview.pump(canvas);  // the thumbnails are in before the card dims them
    preview.tap(Context);
    preview.tap(Down, 2);
    preview.tap(Select);  // Delete: Cancel / Delete, Cancel focused
    preview.pump(canvas);
}

void draw_folder_long_names(UiCanvas& canvas) {
    Preview preview(kLongNames);
    preview.open_folder(kLong);
    preview.pump(canvas);
}

void draw_folder_converting(UiCanvas& canvas) {
    Preview preview(kLibrary | kPhotos);
    preview.jobs.enqueue({preview.tree.inbox() / "IMG_2041.jpg", preview.tree.inbox() / "IMG_2047.jpg"},
                         "sharp", kTrip);
    preview.jobs.tick(FakeJobService::kSecondsPerJob * 0.45);
    preview.open_folder(kTrip);
    preview.pump(canvas);
}

void draw_photos(UiCanvas& canvas) {
    Preview preview(kLibrary | kPhotos, true);
    preview.open_folder(kTrip);
    preview.tap(Left);    // the "+" tile
    preview.tap(Select);  // the picker
    preview.pump(canvas);
    preview.tap(Select);  // first photo
    preview.tap(Right, 2);
    preview.tap(Select);  // third photo
    preview.pump(canvas);
}

void draw_photos_console(UiCanvas& canvas) {
    Preview preview(kLibrary | kPhotos);
    preview.open_folder(kTrip);
    ScreenArgs args;
    args.a = kTrip;
    preview.app.nav().push(ScreenId::Photos, args);
    preview.tap(PageNext);  // Console: the internal and the extended storage
    preview.pump(canvas);
}

void draw_photos_console_folder(UiCanvas& canvas) {
    Preview preview(kLibrary | kPhotos);
    ScreenArgs args;
    args.a = kTrip;
    preview.app.nav().push(ScreenId::Photos, args);
    preview.tap(PageNext);  // Console
    preview.tap(Select);    // internal storage (/data; the app's folder is hidden)
    preview.tap(Select);    // photos/
    preview.tap(Right);
    preview.tap(Select);    // 2026-10/
    preview.tap(Right, 2);
    preview.tap(Select);    // a photo
    preview.pump(canvas);
}

void draw_photos_usb(UiCanvas& canvas) {
    Preview preview(kLibrary);  // no stick: the USB tab says so
    ScreenArgs args;
    args.a = kTrip;
    preview.app.nav().push(ScreenId::Photos, args);
    preview.tap(PageNext, 2);
    preview.pump(canvas);
}

void draw_photos_empty(UiCanvas& canvas) {
    Preview preview(kLibrary);
    ScreenArgs args;
    args.a = kTrip;
    preview.app.nav().push(ScreenId::Photos, args);
    preview.pump(canvas);
}

void draw_convert(UiCanvas& canvas) {
    Preview preview(kLibrary | kPhotos);
    preview.install_models();
    ScreenArgs args;
    args.a = kTrip;
    args.list = {(preview.tree.inbox() / "IMG_2041.jpg").string(),
                 (preview.tree.inbox() / "IMG_2047.jpg").string(),
                 (preview.tree.inbox() / "IMG_2050.jpg").string()};
    preview.open_folder(kTrip);
    preview.app.nav().push(ScreenId::Photos, args);
    preview.app.nav().push(ScreenId::ConvertDialog, args);
    preview.pump(canvas);
}

void draw_convert_none(UiCanvas& canvas) {
    Preview preview(kLibrary | kPhotos);
    preview.models.remove("da2");
    ScreenArgs args;
    args.a = kTrip;
    args.list = {(preview.tree.inbox() / "IMG_2041.jpg").string()};
    preview.app.nav().push(ScreenId::ConvertDialog, args);
    preview.pump(canvas);
}

void draw_settings(UiCanvas& canvas) {
    Preview preview(kLibrary);
    preview.jobs.enqueue({Path("IMG_2041.jpg")}, "sharp", kTrip);
    preview.jobs.tick(1.0);
    preview.tap(Menu);
    preview.pump(canvas);
}

void draw_models(UiCanvas& canvas) {
    Preview preview(kNothing);
    preview.models.accept("sharp");
    preview.models.tick(0.05);  // accepted -> downloading
    preview.models.tick(FakeModelService::kDownloadSeconds * 0.45);
    preview.models.select("da2");
    preview.tap(Menu);
    preview.tap(Select);  // Models
    preview.pump(canvas);
}

void draw_licence(UiCanvas& canvas) {
    Preview preview(kNothing);
    preview.tap(Menu);
    preview.tap(Select);  // Models
    preview.tap(Select);  // SHARP needs its licence
    preview.pump(canvas);
}

void draw_queue(UiCanvas& canvas) {
    Preview preview(kNothing);
    preview.jobs.enqueue({Path("IMG_2041.jpg")}, "sharp", "");
    preview.jobs.enqueue({Path("IMG_2047.jpg"), Path("IMG_2050.jpg")}, "sharp", kTrip);
    preview.jobs.tick(FakeJobService::kSecondsPerJob + 0.1);  // first done, second running
    preview.jobs.tick(2.0);                                   // second at 50 %
    preview.jobs.cancel("fake-job-3");                        // the last one was still queued
    preview.tap(Menu);
    preview.tap(Down);
    preview.tap(Select);  // Conversions
    preview.tap(Down);
    preview.pump(canvas);
}

void open_keyboard(Preview& preview, UiCanvas& canvas) {
    preview.tap(Down);
    preview.tap(Right);
    preview.tap(Select);  // the "New folder" tile
    preview.tap(Down, 2);
    preview.tap(Right, 3);
    preview.pump(canvas);
}

void draw_keyboard(UiCanvas& canvas) {
    Preview preview(kLibrary);
    open_keyboard(preview, canvas);
}

// The same screen as the headset sees it: the VR panel's 896x504 canvas and
// 0.7 theme (ui_setup.cpp), so the keyboard's rows can be checked for clipping
// without a console.
void draw_keyboard_vr(UiCanvas& canvas) {
    Preview preview(kLibrary, false, 896.0f / 1280.0f);
    open_keyboard(preview, canvas);
}

void draw_keyboard_delete(UiCanvas& canvas) {
    Preview preview(kLibrary);
    preview.pump(canvas);  // the covers are in before the dialog dims them
    preview.tap(Right);    // "Empty folder"
    preview.tap(Context);  // Rename, Delete
    preview.tap(Down);
    preview.tap(Select);   // the confirmation over the dimmed folders
    preview.pump(canvas);
}

void draw_move(UiCanvas& canvas) {
    Preview preview(kLibrary);
    preview.open_folder(kTrip);
    preview.tap(Context);
    preview.tap(Select);  // "Move to folder" in the scene's menu
    preview.tap(Down, 2);
    preview.pump(canvas);
}

}  // namespace

const Registry& registry() {
    constexpr uint32_t tv_w = 1280;
    constexpr uint32_t tv_h = 720;
    constexpr float vr_scale = 896.0f / 1280.0f;
    static const Registry patterns = {
        {"pattern", {tv_w, tv_h, 1.0f, draw_test_pattern_screen}},
        {"home", {tv_w, tv_h, 1.0f, draw_home}},
        {"home-empty", {tv_w, tv_h, 1.0f, draw_home_empty}},
        {"home-menu", {tv_w, tv_h, 1.0f, draw_home_menu}},
        {"home-converting", {tv_w, tv_h, 1.0f, draw_home_converting}},
        {"folder", {tv_w, tv_h, 1.0f, draw_folder}},
        {"folder-empty", {tv_w, tv_h, 1.0f, draw_folder_empty}},
        {"folder-many", {tv_w, tv_h, 1.0f, draw_folder_many}},
        {"folder-long-names", {tv_w, tv_h, 1.0f, draw_folder_long_names}},
        {"folder-menu", {tv_w, tv_h, 1.0f, draw_folder_menu}},
        {"folder-delete", {tv_w, tv_h, 1.0f, draw_folder_delete}},
        {"folder-converting", {tv_w, tv_h, 1.0f, draw_folder_converting}},
        {"photos", {tv_w, tv_h, 1.0f, draw_photos}},
        {"photos-console", {tv_w, tv_h, 1.0f, draw_photos_console}},
        {"photos-console-folder", {tv_w, tv_h, 1.0f, draw_photos_console_folder}},
        {"photos-empty", {tv_w, tv_h, 1.0f, draw_photos_empty}},
        {"photos-usb", {tv_w, tv_h, 1.0f, draw_photos_usb}},
        {"convert", {tv_w, tv_h, 1.0f, draw_convert}},
        {"convert-none", {tv_w, tv_h, 1.0f, draw_convert_none}},
        {"settings", {tv_w, tv_h, 1.0f, draw_settings}},
        {"models", {tv_w, tv_h, 1.0f, draw_models}},
        {"licence", {tv_w, tv_h, 1.0f, draw_licence}},
        {"queue", {tv_w, tv_h, 1.0f, draw_queue}},
        {"keyboard", {tv_w, tv_h, 1.0f, draw_keyboard}},
        {"keyboard-vr", {896, 504, vr_scale, draw_keyboard_vr}},
        {"keyboard-delete", {tv_w, tv_h, 1.0f, draw_keyboard_delete}},
        {"move", {tv_w, tv_h, 1.0f, draw_move}},
    };
    return patterns;
}

}  // namespace ui_preview
}  // namespace splat
