// Desktop shell: GLFW window, mouse/WASD camera, mono or side-by-side stereo
// splat viewing through the C6 Vulkan renderer. Headless modes render one
// screenshot or benchmark N frames without a window.
//
//   splat_desktop <scene.psplat|.ply> [options]
//   splat_desktop --scenes DIR [options]
//   splat_desktop --app DATA_DIR [options]     photo-to-VR app mode (D1)
//     --size WxH              eye resolution (default: capture image, else 1280x720)
//     --stereo                side-by-side stereo, IPD 63 mm
//     --screenshot out.png    render one offscreen frame and exit
//     --pose x,y,z,yaw,pitch  head pose, metres and degrees (default 0,0,0,0,0)
//     --bench N               print mean GPU ms per pass over N frames and exit
//     --focal PX              override the capture focal length, in scene pixels
//     --ipd MM                eye separation in stereo (default 63)
//     --smoke N               render N frames, then exit 0
//     --scenes DIR            play a folder of scene folders (scene.psplat in each)
//     --scale S               initial world scale (default 1, clamped 0.25..4)
//     --comfort-radius M      comfort fade radius in metres (default 0.25)
//     --fade-start F          radial fade start, fraction of the radius (default 0.5)
//     --playlist N            headless: play the playlist N loops, one frame per scene
//
// App mode options:
//     --app DATA_DIR          run the upload page + job runner + viewer; the data
//                             directory holds inbox/, scenes/, state/, models/
//     --backend auto|sharp|fallback   conversion backend (default auto)
//     --model FILE.gguf       override the selected backend's converted model
//     --port N                LAN page port (default 8090, 0 picks a free one)
//     --bind ADDR             page address (default: the private LAN address;
//                             there is no implicit 127.0.0.1: pass it here for
//                             a loopback-only page)
//     --headless              no window: run the page and the jobs only
//     --screenshot-dir DIR    headless: render each new scene to DIR/<job>.png
//     --upload PHOTO          upload PHOTO through the LAN page (repeatable)
//     --jobs N                exit after N finished conversions
//     --run-seconds N         exit after N seconds
//     --transition-log FILE   GPU phase + job log (default DATA_DIR/state/transitions.log)
//     --cpu                   force the CPU backend (no GPU device needed)
//
// Mouse look is always on; WASD moves the head. [ and ] select the previous /
// next scene (L1/R1), the scroll wheel or +/- changes world scale, R recentres
// (yaw only) and P pauses. ESC quits.

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "app_mode.h"
#include "common/bytes.h"
#include "common/image.h"
#include "httplib/httplib.h"
#include "ingest/ingest_server.h"
#include "render/scene_playlist.h"
#include "render/splat_renderer.h"
#include "render/viewer_state.h"
#include "render/vulkan_context.h"
#include "scene/ply.h"
#include "scene/psplat.h"
#include "scene/scene_folder.h"
#include "scene/splat_scene.h"

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;

// Ctrl-C / SIGTERM. The handler does one thing: it records which signal asked
// for the stop (lock-free atomic store, nothing that allocates or locks). The
// app-mode loops poll the flag on their next tick and leave through the normal
// path — stop the page, join the worker, unload the model, destroy the
// renderer — instead of the process dying inside a Vulkan call or a write.
std::atomic<bool> g_stop_requested{false};
std::atomic<int> g_stop_signal{0};

// The handler stores into these, so they have to be lock-free (a lock/unlock
// inside a signal handler is not safe).
static_assert(std::atomic<bool>::is_always_lock_free, "the handler must not take a lock");
static_assert(std::atomic<int>::is_always_lock_free, "the handler must not take a lock");

void handle_stop_signal(int signal) {
    g_stop_signal.store(signal, std::memory_order_relaxed);
    g_stop_requested.store(true, std::memory_order_relaxed);
}

const char* stop_signal_name(int signal) {
    if (signal == SIGINT) return "SIGINT";
    if (signal == SIGTERM) return "SIGTERM";
    return "other";
}

// One line in the transition log, so the reason for the teardown is recorded
// next to the phase changes it caused.
void log_stop_requested(splat::AppMode& app) {
    app.log_transition("app.signal",
                       std::string("signal=") + stop_signal_name(g_stop_signal.load()));
}

// SHARP's .ply may carry no intrinsic element, in which case the loader leaves
// the struct zeroed.
bool intrinsics_valid(const splat::Intrinsics& intrinsics) {
    return intrinsics.fx > 0.0f && intrinsics.fy > 0.0f && intrinsics.width > 0 &&
           intrinsics.height > 0;
}

struct Options {
    std::string scene_path;
    std::string scenes_dir;
    uint32_t width = 0;
    uint32_t height = 0;
    bool stereo = false;
    std::string screenshot;
    bool have_pose = false;
    splat::CameraPose pose;
    int bench_frames = 0;
    int smoke_frames = -1;
    float focal = 0.0f;
    float ipd_mm = 63.0f;
    float world_scale = 1.0f;
    float comfort_radius = splat::kDefaultComfortRadius;
    float fade_start = splat::kDefaultFadeStart;
    int playlist_loops = 0;

    // App mode (--app). Empty app_data_dir keeps the single-scene viewer.
    std::string app_data_dir;
    std::string app_backend = "auto";
    std::string app_model;
    std::string app_bind;
    std::string screenshot_dir;
    std::string transition_log;
    std::vector<std::string> uploads;
    int exit_after_jobs = -1;
    int run_seconds = 0;
    bool headless = false;
    bool cpu = false;
    uint16_t port = 8090;
};

double now_ms() {
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point start = Clock::now();
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string vk_error(const char* what, VkResult result) {
    return std::string(what) + " failed (VkResult " + std::to_string(static_cast<int>(result)) +
           ")";
}

void glfw_error(int code, const char* description) {
    std::fprintf(stderr, "glfw error %d: %s\n", code, description);
}

splat::Result<splat::LoadedScene> load_scene(const std::string& path) {
    return splat::read_scene_path(splat::Path(path));
}

splat::CameraIntrinsics eye_intrinsics(const splat::Intrinsics& capture, float focal,
                                       uint32_t width, uint32_t height) {
    splat::CameraIntrinsics out;
    if (intrinsics_valid(capture)) {
        const float sx = static_cast<float>(width) / static_cast<float>(capture.width);
        const float sy = static_cast<float>(height) / static_cast<float>(capture.height);
        const float fx = focal > 0.0f ? focal : capture.fx;
        const float fy = focal > 0.0f ? focal : capture.fy;
        return {fx * sx, fy * sy, capture.cx * sx, capture.cy * sy};
    }
    if (focal > 0.0f) {
        return {focal, focal, 0.5f * width, 0.5f * height};
    }
    return out;
}

bool parse_size(const char* text, uint32_t& width, uint32_t& height) {
    unsigned w = 0;
    unsigned h = 0;
    if (std::sscanf(text, "%ux%u", &w, &h) != 2 && std::sscanf(text, "%uX%u", &w, &h) != 2) {
        return false;
    }
    if (w == 0 || h == 0) return false;
    width = w;
    height = h;
    return true;
}

bool parse_pose(const char* text, splat::CameraPose& pose) {
    float x = 0;
    float y = 0;
    float z = 0;
    float yaw = 0;
    float pitch = 0;
    if (std::sscanf(text, "%f,%f,%f,%f,%f", &x, &y, &z, &yaw, &pitch) != 5) return false;
    pose.position[0] = x;
    pose.position[1] = y;
    pose.position[2] = z;
    pose.yaw = yaw * kDegToRad;
    pose.pitch = pitch * kDegToRad;
    return true;
}

void print_usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s <scene.psplat|.ply> [--stereo] [--size WxH] [--screenshot out.png]\n"
                 "          [--pose x,y,z,yaw,pitch] [--bench N] [--smoke N] [--focal PX]\n"
                 "          [--ipd MM] [--scale S] [--comfort-radius M] [--fade-start F]\n"
                 "       %s --scenes DIR [--playlist N] [options]\n"
                 "       %s --app DATA_DIR [--backend auto|sharp|fallback] [--port N]\n"
                 "          [--bind ADDR] [--headless] [--screenshot-dir DIR] [--upload PHOTO]\n"
                 "          [--jobs N] [--run-seconds N] [--transition-log FILE] [--cpu]\n"
                 "          [--model GGUF]\n",
                 argv0, argv0, argv0);
}

bool parse_options(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (std::strcmp(arg, "--size") == 0) {
            const char* value = next(arg);
            if (value == nullptr || !parse_size(value, options.width, options.height)) return false;
        } else if (std::strcmp(arg, "--stereo") == 0) {
            options.stereo = true;
        } else if (std::strcmp(arg, "--screenshot") == 0) {
            const char* value = next(arg);
            if (value == nullptr) return false;
            options.screenshot = value;
        } else if (std::strcmp(arg, "--pose") == 0) {
            const char* value = next(arg);
            if (value == nullptr || !parse_pose(value, options.pose)) return false;
            options.have_pose = true;
        } else if (std::strcmp(arg, "--bench") == 0) {
            const char* value = next(arg);
            if (value == nullptr) return false;
            options.bench_frames = std::atoi(value);
            if (options.bench_frames <= 0) return false;
        } else if (std::strcmp(arg, "--smoke") == 0) {
            const char* value = next(arg);
            if (value == nullptr) return false;
            options.smoke_frames = std::atoi(value);
            if (options.smoke_frames < 0) return false;
        } else if (std::strcmp(arg, "--focal") == 0) {
            const char* value = next(arg);
            if (value == nullptr) return false;
            options.focal = static_cast<float>(std::atof(value));
            if (options.focal <= 0.0f) return false;
        } else if (std::strcmp(arg, "--ipd") == 0) {
            const char* value = next(arg);
            if (value == nullptr) return false;
            options.ipd_mm = static_cast<float>(std::atof(value));
            if (options.ipd_mm <= 0.0f) return false;
        } else if (std::strcmp(arg, "--scenes") == 0) {
            const char* value = next(arg);
            if (value == nullptr) return false;
            options.scenes_dir = value;
        } else if (std::strcmp(arg, "--scale") == 0) {
            const char* value = next(arg);
            if (value == nullptr) return false;
            options.world_scale = static_cast<float>(std::atof(value));
            if (options.world_scale <= 0.0f) return false;
        } else if (std::strcmp(arg, "--comfort-radius") == 0) {
            const char* value = next(arg);
            if (value == nullptr) return false;
            options.comfort_radius = static_cast<float>(std::atof(value));
            if (options.comfort_radius <= 0.0f) return false;
        } else if (std::strcmp(arg, "--fade-start") == 0) {
            const char* value = next(arg);
            if (value == nullptr) return false;
            options.fade_start = static_cast<float>(std::atof(value));
            if (options.fade_start < 0.0f || options.fade_start > 1.0f) return false;
        } else if (std::strcmp(arg, "--playlist") == 0) {
            const char* value = next(arg);
            if (value == nullptr) return false;
            options.playlist_loops = std::atoi(value);
            if (options.playlist_loops <= 0) return false;
        } else if (std::strcmp(arg, "--app") == 0) {
            const char* value = next(arg);
            if (value == nullptr) return false;
            options.app_data_dir = value;
        } else if (std::strcmp(arg, "--backend") == 0) {
            const char* value = next(arg);
            if (value == nullptr) return false;
            options.app_backend = value;
            if (options.app_backend != "auto" && options.app_backend != "sharp" &&
                options.app_backend != "fallback") {
                std::fprintf(stderr, "--backend must be auto, sharp or fallback\n");
                return false;
            }
        } else if (std::strcmp(arg, "--model") == 0) {
            const char* value = next(arg);
            if (value == nullptr) return false;
            options.app_model = value;
        } else if (std::strcmp(arg, "--port") == 0) {
            const char* value = next(arg);
            if (value == nullptr) return false;
            const int port = std::atoi(value);
            if (port < 0 || port > 65535) return false;
            options.port = static_cast<uint16_t>(port);
        } else if (std::strcmp(arg, "--bind") == 0) {
            const char* value = next(arg);
            if (value == nullptr) return false;
            options.app_bind = value;
        } else if (std::strcmp(arg, "--headless") == 0) {
            options.headless = true;
        } else if (std::strcmp(arg, "--screenshot-dir") == 0) {
            const char* value = next(arg);
            if (value == nullptr) return false;
            options.screenshot_dir = value;
        } else if (std::strcmp(arg, "--upload") == 0) {
            const char* value = next(arg);
            if (value == nullptr) return false;
            options.uploads.emplace_back(value);
        } else if (std::strcmp(arg, "--jobs") == 0) {
            const char* value = next(arg);
            if (value == nullptr) return false;
            options.exit_after_jobs = std::atoi(value);
            if (options.exit_after_jobs < 0) return false;
        } else if (std::strcmp(arg, "--run-seconds") == 0) {
            const char* value = next(arg);
            if (value == nullptr) return false;
            options.run_seconds = std::atoi(value);
            if (options.run_seconds < 0) return false;
        } else if (std::strcmp(arg, "--transition-log") == 0) {
            const char* value = next(arg);
            if (value == nullptr) return false;
            options.transition_log = value;
        } else if (std::strcmp(arg, "--cpu") == 0) {
            options.cpu = true;
        } else if (arg[0] == '-') {
            print_usage(argv[0]);
            return false;
        } else if (options.scene_path.empty()) {
            options.scene_path = arg;
        } else {
            print_usage(argv[0]);
            return false;
        }
    }
    if (!options.scene_path.empty() && !options.scenes_dir.empty()) {
        std::fprintf(stderr, "give either a scene file or --scenes, not both\n");
        return false;
    }
    if (!options.app_data_dir.empty()) {
        if (!options.scene_path.empty() || !options.scenes_dir.empty()) {
            std::fprintf(stderr, "--app does not take a scene file or --scenes\n");
            return false;
        }
        if (!options.screenshot.empty() || options.bench_frames > 0 ||
            options.playlist_loops > 0) {
            std::fprintf(stderr,
                         "--app renders the scenes it converts; use --screenshot-dir, not "
                         "--screenshot/--bench/--playlist\n");
            return false;
        }
        return true;
    }
    if (options.headless || !options.uploads.empty() || options.exit_after_jobs >= 0 ||
        options.run_seconds > 0 || !options.screenshot_dir.empty() ||
        !options.transition_log.empty()) {
        std::fprintf(stderr, "that option needs --app\n");
        return false;
    }
    return !options.scene_path.empty() || !options.scenes_dir.empty();
}

std::unique_ptr<splat::SplatRenderer> make_renderer(splat::VulkanContext& context,
                                                    const Options& options, uint32_t eye_width,
                                                    uint32_t eye_height,
                                                    const splat::Intrinsics& capture,
                                                    VkFormat format,
                                                    const splat::SplatScene& scene,
                                                    bool show_info) {
    splat::RendererOptions renderer_options;
    renderer_options.shader_dir = SPLAT_SHADER_DIR;
    renderer_options.eye_width = eye_width;
    renderer_options.eye_height = eye_height;
    renderer_options.stereo = options.stereo;
    renderer_options.target_format = format;
    renderer_options.intrinsics =
        eye_intrinsics(capture, options.focal, eye_width, eye_height);
    // The original photo's frustum, for the comfort pass that fades the
    // extrapolated region beyond what the capture camera saw.
    if (intrinsics_valid(capture)) {
        renderer_options.capture_intrinsics = {capture.fx, capture.fy, capture.cx, capture.cy};
        renderer_options.capture_width = capture.width;
        renderer_options.capture_height = capture.height;
    }
    auto renderer = splat::SplatRenderer::create(context, renderer_options, scene);
    if (!renderer.ok()) {
        std::fprintf(stderr, "splat_desktop: %s\n", renderer.message().c_str());
        return nullptr;
    }
    if (show_info && intrinsics_valid(capture)) {
        std::printf("splat_desktop: capture intrinsics fx %.2f fy %.2f cx %.1f cy %.1f, image %ux%u\n",
                    capture.fx, capture.fy, capture.cx, capture.cy, capture.width,
                    capture.height);
    }
    return renderer.take();
}

int run_headless(const Options& options, const splat::LoadedScene& load,
                 splat::ScenePlaylist* playlist, double start_ms) {
    uint32_t eye_width = options.width;
    uint32_t eye_height = options.height;
    if (eye_width == 0 || eye_height == 0) {
        if (intrinsics_valid(load.meta.intrinsics)) {
            eye_width = load.meta.intrinsics.width;
            eye_height = load.meta.intrinsics.height;
        } else {
            eye_width = 1280;
            eye_height = 720;
        }
    }

    splat::VulkanContextOptions context_options;
    auto context = splat::VulkanContext::create(context_options);
    if (!context.ok()) {
        std::fprintf(stderr, "splat_desktop: %s\n", context.message().c_str());
        return 1;
    }
    splat::VulkanContext& device = *context.value();
    std::printf("splat_desktop: %s, Vulkan %u.%u.%u\n", device.properties().deviceName,
                VK_VERSION_MAJOR(device.properties().apiVersion),
                VK_VERSION_MINOR(device.properties().apiVersion),
                VK_VERSION_PATCH(device.properties().apiVersion));

    // sRGB, not UNORM: the fixed-function blend then works in linear light, as
    // ml-sharp's renderer does, and the readback bytes are already sRGB-encoded
    // for the PNG. A UNORM target would blend the encoded values instead and
    // darken the mid-tones by about 1 %.
    const VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;
    auto renderer = make_renderer(device, options, eye_width, eye_height, load.meta.intrinsics,
                                  format, load.scene, true);
    if (!renderer) return 1;

    const VkExtent2D extent = renderer->target_extent();
    auto image = splat::create_vulkan_color_image(
        device, format, extent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!image.ok()) {
        std::fprintf(stderr, "splat_desktop: %s\n", image.message().c_str());
        return 1;
    }

    splat::ViewerState state;
    state.set_world_scale(options.world_scale);
    state.set_comfort_radius(options.comfort_radius);
    state.set_fade_start(options.fade_start);
    const float* head = options.pose.position;
    splat::FrameView view =
        state.frame(head, options.pose.yaw, options.pose.pitch, options.ipd_mm * 0.001f);

    splat::RenderTarget target;
    target.image = image.value().image;
    target.view = image.value().view;
    target.extent = extent;
    target.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    target.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    // Headless playlist benchmark: the old scene renders while the next file
    // is read on the preload thread, then stage + commit swaps it in.
    if (options.playlist_loops > 0 && playlist != nullptr) {
        const size_t count = playlist->size();
        const int steps = options.playlist_loops * static_cast<int>(count);
        splat::ScenePreloader preloader;
        size_t display = playlist->index();
        if (count > 1) {
            preloader.start(playlist->entry((display + 1) % count).scene_file);
        }
        double previous_preload_ms = 0.0;
        double scene_start_ms = start_ms;
        for (int step = 0; step < steps; ++step) {
            if (step > 0) {
                if (count <= 1) break;
                while (!preloader.ready() && !preloader.failed()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                if (preloader.failed()) {
                    std::fprintf(stderr, "splat_desktop: %s\n", preloader.error().c_str());
                    return 1;
                }
                previous_preload_ms = preloader.last_load_ms();
                auto loaded = preloader.take();
                if (!loaded.ok()) {
                    std::fprintf(stderr, "splat_desktop: %s\n", loaded.message().c_str());
                    return 1;
                }
                const double stage_start = now_ms();
                const splat::Status staged = renderer->stage_scene(loaded.value().scene);
                const double stage_ms = now_ms() - stage_start;
                if (!staged.ok()) {
                    std::fprintf(stderr, "splat_desktop: %s\n", staged.message().c_str());
                    return 1;
                }
                const double staged_mib =
                    static_cast<double>(renderer->staged_bytes()) / (1024.0 * 1024.0);
                const splat::Status committed = renderer->commit_staged_scene();
                if (!committed.ok()) {
                    std::fprintf(stderr, "splat_desktop: %s\n", committed.message().c_str());
                    return 1;
                }
                display = (display + 1) % count;
                playlist->set_index(display);
                std::printf("[playlist] preload %.1f ms  stage %.1f ms  commit %.2f ms  "
                            "staged %.1f MiB\n",
                            previous_preload_ms, stage_ms, renderer->last_commit_ms(), staged_mib);
            }
            const double frame_start = now_ms();
            const splat::Status status = renderer->render(view, target, false);
            if (!status.ok()) {
                std::fprintf(stderr, "splat_desktop: %s\n", status.message().c_str());
                return 1;
            }
            target.initial_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            const double first_frame_ms = now_ms() - scene_start_ms;
            std::printf("[playlist] %s  preload %.1f ms  first frame %.1f ms  project %.2f  "
                        "sort %.2f  draw %.2f  visible %u\n",
                        playlist->current().folder.filename().string().c_str(),
                        step == 0 ? 0.0 : previous_preload_ms, first_frame_ms,
                        renderer->timings().project_ms, renderer->timings().sort_ms,
                        renderer->timings().draw_ms, renderer->last_visible_count());
            if (step + 1 < steps && count > 1) {
                scene_start_ms = now_ms();
                // The preload started before the loop already covers this
                // step; restarting it would discard that result and reread
                // the same file from disk.
                const splat::Path next_path =
                    playlist->entry((display + 1) % count).scene_file;
                const bool already_loading =
                    (preloader.running() || preloader.ready()) && preloader.path() == next_path;
                if (!already_loading) preloader.start(next_path);
            }
        }
        splat::destroy_vulkan_image(device, image.value());
        return 0;
    }

    const bool screenshot = !options.screenshot.empty();
    const int frames = screenshot ? 1 : options.bench_frames;
    splat::PassTimings total;

    for (int frame = 0; frame < frames; ++frame) {
        const splat::Status status = renderer->render(view, target, screenshot);
        if (!status.ok()) {
            std::fprintf(stderr, "splat_desktop: %s\n", status.message().c_str());
            return 1;
        }
        target.initial_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        if (frame > 0) {  // frame 0 is a warm-up in bench mode
            total.project_ms += renderer->timings().project_ms;
            total.sort_ms += renderer->timings().sort_ms;
            total.draw_ms += renderer->timings().draw_ms;
        }
    }

    if (screenshot) {
        auto image_data = renderer->read_image();
        if (!image_data.ok()) {
            std::fprintf(stderr, "splat_desktop: %s\n", image_data.message().c_str());
            return 1;
        }
        const double ready_ms = now_ms();
        const splat::Image& output = image_data.value();
        if (stbi_write_png(options.screenshot.c_str(), output.width, output.height, 3,
                           output.rgb.data(), output.width * 3) == 0) {
            std::fprintf(stderr, "splat_desktop: cannot write %s\n", options.screenshot.c_str());
            return 1;
        }
        std::printf("splat_desktop: wrote %s (%dx%d), visible %u, project %.2f ms, sort %.2f ms, "
                    "draw %.2f ms, comfort %.3f, scale %.3f\n",
                    options.screenshot.c_str(), output.width, output.height,
                    renderer->last_visible_count(), renderer->timings().project_ms,
                    renderer->timings().sort_ms, renderer->timings().draw_ms,
                    view.comfort_fade, view.scene_scale);
        std::printf("splat_desktop: load-to-first-frame %.1f ms\n", ready_ms - start_ms);
    } else {
        const double divisor = frames > 1 ? frames - 1 : 1;
        std::printf("splat_desktop: bench %d frames at %ux%u per eye%s, visible %u\n",
                    frames, eye_width, eye_height, options.stereo ? " (stereo)" : "",
                    renderer->last_visible_count());
        std::printf("splat_desktop: mean GPU ms  project %.3f  sort %.3f  draw %.3f  total %.3f\n",
                    total.project_ms / divisor, total.sort_ms / divisor,
                    total.draw_ms / divisor,
                    (total.project_ms + total.sort_ms + total.draw_ms) / divisor);
    }

    splat::destroy_vulkan_image(device, image.value());
    return 0;
}

// Viewer ---------------------------------------------------------------------

struct Viewer {
    GLFWwindow* window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    std::unique_ptr<splat::VulkanContext> context;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    VkFence acquire_fence = VK_NULL_HANDLE;
};

bool has_extension(const std::vector<VkExtensionProperties>& available, const char* name) {
    for (const auto& extension : available) {
        if (std::strcmp(extension.extensionName, name) == 0) return true;
    }
    return false;
}

bool create_instance(Viewer& viewer, std::string& error) {
    uint32_t glfw_count = 0;
    const char** glfw_extensions = glfwGetRequiredInstanceExtensions(&glfw_count);
    if (glfw_extensions == nullptr) {
        error = "GLFW did not provide Vulkan instance extensions";
        return false;
    }

    uint32_t available_count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &available_count, nullptr);
    std::vector<VkExtensionProperties> available(available_count);
    vkEnumerateInstanceExtensionProperties(nullptr, &available_count, available.data());

    // The portability flag is only legal together with the extension
    // (VUID-VkInstanceCreateInfo-flags-06776), so one flag drives both.
    bool portability = false;
#if defined(__APPLE__)
    portability = has_extension(available, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif
    std::vector<const char*> extensions(glfw_extensions, glfw_extensions + glfw_count);
    if (portability) extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "splat_desktop";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "ps5-splat-vr";
    app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app_info;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    if (portability) info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;

    const VkResult result = vkCreateInstance(&info, nullptr, &viewer.instance);
    if (result != VK_SUCCESS) {
        error = vk_error("vkCreateInstance", result);
        return false;
    }
    return true;
}

bool create_swapchain(Viewer& viewer, uint32_t width, uint32_t height, std::string& error) {
    VkSurfaceCapabilitiesKHR capabilities{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(viewer.context->physical_device(), viewer.surface,
                                              &capabilities);

    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(viewer.context->physical_device(), viewer.surface,
                                         &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(viewer.context->physical_device(), viewer.surface,
                                         &format_count, formats.data());
    if (formats.empty()) {
        error = "surface reports no formats";
        return false;
    }

    // An sRGB swapchain image decodes on load and encodes on store, so the
    // renderer's premultiplied blend runs in linear light (matching ml-sharp).
    // A UNORM surface keeps the renderer in encoded space and looks slightly
    // dark in the mid-tones; it is only the fallback.
    VkSurfaceFormatKHR surface_format = formats[0];
    bool found_srgb = false;
    for (const auto& candidate : formats) {
        if ((candidate.format == VK_FORMAT_B8G8R8A8_SRGB ||
             candidate.format == VK_FORMAT_R8G8B8A8_SRGB) &&
            candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            surface_format = candidate;
            found_srgb = true;
            break;
        }
    }
    if (!found_srgb) {
        for (const auto& candidate : formats) {
            if ((candidate.format == VK_FORMAT_B8G8R8A8_UNORM ||
                 candidate.format == VK_FORMAT_R8G8B8A8_UNORM) &&
                candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                surface_format = candidate;
                break;
            }
        }
        std::fprintf(stderr,
                     "splat_desktop: the surface offers no sRGB format; the viewer blends in "
                     "encoded (non-linear) space\n");
    }

    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = std::clamp(width, capabilities.minImageExtent.width,
                                  capabilities.maxImageExtent.width);
        extent.height = std::clamp(height, capabilities.minImageExtent.height,
                                   capabilities.maxImageExtent.height);
    }

    uint32_t image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount) {
        image_count = capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = viewer.surface;
    info.minImageCount = image_count;
    info.imageFormat = surface_format.format;
    info.imageColorSpace = surface_format.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    const uint32_t families[] = {viewer.context->queue_family(),
                                 viewer.context->present_family()};
    if (viewer.context->queue_family() != viewer.context->present_family()) {
        info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = families;
    } else {
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;

    VkResult result = vkCreateSwapchainKHR(viewer.context->device(), &info, nullptr,
                                           &viewer.swapchain);
    if (result != VK_SUCCESS) {
        error = vk_error("vkCreateSwapchainKHR", result);
        return false;
    }
    viewer.format = surface_format.format;
    viewer.extent = extent;

    uint32_t actual_count = 0;
    vkGetSwapchainImagesKHR(viewer.context->device(), viewer.swapchain, &actual_count, nullptr);
    viewer.images.resize(actual_count);
    vkGetSwapchainImagesKHR(viewer.context->device(), viewer.swapchain, &actual_count,
                            viewer.images.data());

    viewer.views.resize(actual_count, VK_NULL_HANDLE);
    for (size_t i = 0; i < viewer.images.size(); ++i) {
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = viewer.images[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = viewer.format;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        result = vkCreateImageView(viewer.context->device(), &view_info, nullptr,
                                   &viewer.views[i]);
        if (result != VK_SUCCESS) {
            error = vk_error("vkCreateImageView", result);
            return false;
        }
    }
    return true;
}

bool create_viewer_sync(Viewer& viewer, std::string& error) {
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    const VkResult result = vkCreateFence(viewer.context->device(), &fence_info, nullptr,
                                          &viewer.acquire_fence);
    if (result != VK_SUCCESS) {
        error = vk_error("vkCreateFence", result);
        return false;
    }
    return true;
}

struct HeadState {
    float position[3] = {0.0f, 0.0f, 0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
    double last_x = 0.0;
    double last_y = 0.0;
    bool have_last = false;
};

void update_head(HeadState& head, GLFWwindow* window, float dt) {
    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(window, &x, &y);
    if (head.have_last) {
        head.yaw += static_cast<float>(x - head.last_x) * 0.0025f;
        head.pitch += static_cast<float>(y - head.last_y) * 0.0025f;
        const float limit = 80.0f * kDegToRad;
        head.pitch = std::clamp(head.pitch, -limit, limit);
    }
    head.last_x = x;
    head.last_y = y;
    head.have_last = true;

    const float yaw = head.yaw;
    const float pitch = head.pitch;
    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);
    const float forward[3] = {sy * cp, sp, cy * cp};
    const float right[3] = {cy, 0.0f, -sy};

    float move[3] = {0.0f, 0.0f, 0.0f};
    const float speed = 1.5f * dt;
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {
        for (int i = 0; i < 3; ++i) move[i] += forward[i] * speed;
    }
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) {
        for (int i = 0; i < 3; ++i) move[i] -= forward[i] * speed;
    }
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
        for (int i = 0; i < 3; ++i) move[i] += right[i] * speed;
    }
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) {
        for (int i = 0; i < 3; ++i) move[i] -= right[i] * speed;
    }
    for (int i = 0; i < 3; ++i) head.position[i] += move[i];

    // Do not hard-clamp at the comfort radius: moving beyond it fades to the
    // backdrop instead of showing torn geometry. Only stop the head running
    // away entirely.
    const float limit = 10.0f;
    const float length = std::sqrt(head.position[0] * head.position[0] +
                                   head.position[1] * head.position[1] +
                                   head.position[2] * head.position[2]);
    if (length > limit && length > 0.0f) {
        for (int i = 0; i < 3; ++i) head.position[i] *= limit / length;
    }
}

double g_scroll = 0.0;

void scroll_callback(GLFWwindow*, double, double y_offset) { g_scroll += y_offset; }

void destroy_viewer(Viewer& viewer);

int run_viewer(const Options& options, const splat::LoadedScene& load,
               splat::ScenePlaylist* playlist) {
    Viewer viewer;
    std::string error;

    uint32_t requested_width = options.width;
    uint32_t requested_height = options.height;
    if (requested_width == 0 || requested_height == 0) {
        if (intrinsics_valid(load.meta.intrinsics)) {
            requested_width = load.meta.intrinsics.width;
            requested_height = load.meta.intrinsics.height;
        } else {
            requested_width = 1280;
            requested_height = 720;
        }
    }
    if (options.stereo) requested_width *= 2;

    glfwSetErrorCallback(glfw_error);
    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    viewer.window = glfwCreateWindow(static_cast<int>(requested_width),
                                     static_cast<int>(requested_height),
                                     "Photo Splat VR", nullptr, nullptr);
    if (viewer.window == nullptr) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }
    glfwSetScrollCallback(viewer.window, scroll_callback);
    glfwSetInputMode(viewer.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    bool ok = create_instance(viewer, error);
    if (ok) {
        ok = glfwCreateWindowSurface(viewer.instance, viewer.window, nullptr, &viewer.surface) ==
             VK_SUCCESS;
        if (!ok) error = "glfwCreateWindowSurface failed";
    }
    if (ok) {
        splat::VulkanContextOptions context_options;
        context_options.instance = viewer.instance;
        context_options.surface = viewer.surface;
        context_options.present = true;
        auto context = splat::VulkanContext::create(context_options);
        if (!context.ok()) {
            error = context.message();
            ok = false;
        } else {
            viewer.context = context.take();
        }
    }

    int framebuffer_width = 0;
    int framebuffer_height = 0;
    glfwGetFramebufferSize(viewer.window, &framebuffer_width, &framebuffer_height);
    if (ok) {
        ok = create_swapchain(viewer, static_cast<uint32_t>(framebuffer_width),
                              static_cast<uint32_t>(framebuffer_height), error);
    }
    if (ok) {
        std::printf("splat_desktop: %s, Vulkan %u.%u, swapchain %ux%u %s\n",
                    viewer.context->properties().deviceName,
                    VK_VERSION_MAJOR(viewer.context->properties().apiVersion),
                    VK_VERSION_MINOR(viewer.context->properties().apiVersion),
                    viewer.extent.width, viewer.extent.height,
                    viewer.format == VK_FORMAT_B8G8R8A8_SRGB ||
                            viewer.format == VK_FORMAT_R8G8B8A8_SRGB
                        ? "sRGB"
                        : "UNORM");
        std::printf("controls: mouse look, WASD move, [ ] previous/next scene, scroll or +/- "
                    "scale, R recentre, P pause, ESC quit\n");
    }
    if (ok) ok = create_viewer_sync(viewer, error);

    const uint32_t eye_width = viewer.extent.width / (options.stereo ? 2u : 1u);
    const uint32_t eye_height = viewer.extent.height;
    std::unique_ptr<splat::SplatRenderer> renderer;
    if (ok) {
        renderer = make_renderer(*viewer.context, options, eye_width, eye_height,
                                 load.meta.intrinsics, viewer.format, load.scene, true);
        if (!renderer) ok = false;
    }

    splat::ViewerState state;
    state.set_world_scale(options.world_scale);
    state.set_comfort_radius(options.comfort_radius);
    state.set_fade_start(options.fade_start);

    HeadState head;
    head.position[0] = options.pose.position[0];
    head.position[1] = options.pose.position[1];
    head.position[2] = options.pose.position[2];
    head.yaw = options.pose.yaw;
    head.pitch = options.pose.pitch;

    const size_t playlist_size = playlist ? playlist->size() : 0;
    size_t display_index = playlist ? playlist->index() : 0;
    long target_index = static_cast<long>(display_index);
    long staged_index = -1;
    // A scene that failed to load is remembered so the viewer does not retry
    // it (and log) every frame; a successful commit clears it.
    splat::Path failed_path;
    splat::ScenePreloader preloader;

    auto scene_index_of = [&](const splat::Path& path) -> long {
        if (playlist == nullptr) return -1;
        for (size_t i = 0; i < playlist->size(); ++i) {
            if (playlist->entry(i).scene_file == path) return static_cast<long>(i);
        }
        return -1;
    };
    auto scene_path_at = [&](long index) -> splat::Path {
        return playlist->entry(static_cast<size_t>(index)).scene_file;
    };

    // Start loading the next scene while the first one is on screen.
    if (playlist_size > 1) {
        preloader.start(scene_path_at((static_cast<long>(display_index) + 1) %
                                      static_cast<long>(playlist_size)));
    }

    bool key_down[GLFW_KEY_LAST + 1] = {};
    auto pressed = [&](int key) {
        const bool down = glfwGetKey(viewer.window, key) == GLFW_PRESS;
        const bool edge = down && !key_down[key];
        key_down[key] = down;
        return edge;
    };

    splat::FrameView view;
    view.ipd = options.ipd_mm * 0.001f;

    int rendered = 0;
    double last_time = glfwGetTime();
    double last_print = last_time;
    while (ok) {
        glfwPollEvents();
        if (glfwGetKey(viewer.window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
        if (glfwWindowShouldClose(viewer.window) != 0) break;
        if (options.smoke_frames >= 0 && rendered >= options.smoke_frames) break;

        const double now = glfwGetTime();
        const float dt = static_cast<float>(now - last_time);
        last_time = now;

        if (pressed(GLFW_KEY_P)) state.set_paused(!state.paused());

        if (!state.paused()) {
            update_head(head, viewer.window, dt);

            if (pressed(GLFW_KEY_R)) {
                state.recentre(head.position, head.yaw);
                std::printf("[viewer] recentre: anchor %.3f %.3f %.3f, yaw offset %.1f deg\n",
                            state.anchor()[0], state.anchor()[1], state.anchor()[2],
                            head.yaw / kDegToRad);
                std::fflush(stdout);
            }
            if (playlist_size > 0 && pressed(GLFW_KEY_RIGHT_BRACKET)) {
                target_index = (static_cast<long>(display_index) + 1) %
                               static_cast<long>(playlist_size);
            }
            if (playlist_size > 0 && pressed(GLFW_KEY_LEFT_BRACKET)) {
                target_index = (static_cast<long>(display_index) +
                                static_cast<long>(playlist_size) - 1) %
                               static_cast<long>(playlist_size);
            }

            if (pressed(GLFW_KEY_EQUAL) || pressed(GLFW_KEY_KP_ADD)) state.scale_world(1.1f);
            if (pressed(GLFW_KEY_MINUS) || pressed(GLFW_KEY_KP_SUBTRACT)) {
                state.scale_world(1.0f / 1.1f);
            }
            if (std::abs(g_scroll) > 1.0e-6) {
                state.scale_world(std::exp(static_cast<float>(g_scroll) * 0.1f));
                g_scroll = 0.0;
            }
        }

        // Scene swap: staging happens when the preload is ready, so pressing
        // the key commits an already-uploaded buffer. The old scene renders
        // until the commit, and commit only rewrites descriptors.
        if (playlist_size > 0 && target_index != static_cast<long>(display_index)) {
            const splat::Path target_path = scene_path_at(target_index);
            if (staged_index == target_index) {
                const splat::Status committed = renderer->commit_staged_scene();
                if (!committed.ok()) {
                    error = committed.message();
                    ok = false;
                    break;
                }
                display_index = static_cast<size_t>(target_index);
                playlist->set_index(display_index);
                staged_index = -1;
                failed_path = splat::Path{};
                std::printf("[viewer] scene %zu/%zu %s  commit %.2f ms\n", display_index + 1,
                            playlist_size, playlist->current().folder.filename().string().c_str(),
                            renderer->last_commit_ms());
                std::fflush(stdout);
            } else if (preloader.ready() && preloader.path() == target_path) {
                auto loaded = preloader.take();
                if (!loaded.ok()) {
                    std::fprintf(stderr, "splat_desktop: %s\n", loaded.message().c_str());
                } else {
                    if (renderer->has_staged_scene()) renderer->discard_staged_scene();
                    const double stage_start = now_ms();
                    const splat::Status staged = renderer->stage_scene(loaded.value().scene);
                    if (!staged.ok()) {
                        std::fprintf(stderr, "splat_desktop: %s\n", staged.message().c_str());
                    } else {
                        const double staged_mib = static_cast<double>(renderer->staged_bytes()) /
                                                  (1024.0 * 1024.0);
                        const splat::Status committed = renderer->commit_staged_scene();
                        if (!committed.ok()) {
                            error = committed.message();
                            ok = false;
                            break;
                        }
                        display_index = static_cast<size_t>(target_index);
                        playlist->set_index(display_index);
                        staged_index = -1;
                        failed_path = splat::Path{};
                        std::printf("[viewer] scene %zu/%zu %s  preload %.1f ms  stage %.1f ms  "
                                    "commit %.2f ms  staged %.1f MiB\n",
                                    display_index + 1, playlist_size,
                                    playlist->current().folder.filename().string().c_str(),
                                    preloader.last_load_ms(), now_ms() - stage_start,
                                    renderer->last_commit_ms(), staged_mib);
                        std::fflush(stdout);
                    }
                }
            } else if (preloader.failed() && preloader.path() == target_path) {
                // Log the failure once, then stay on the current scene instead
                // of restarting the failing read every frame.
                std::fprintf(stderr, "splat_desktop: preload failed: %s\n",
                             preloader.error().c_str());
                failed_path = target_path;
                preloader.cancel();
            } else if (target_path != failed_path) {
                // Not ready for this target (e.g. previous rather than the
                // prefetched next): drop prefetch and start the requested one.
                // If the worker is already loading exactly this target, wait.
                if (!preloader.running() || preloader.path() != target_path) {
                    if (preloader.running() || preloader.ready()) preloader.cancel();
                    if (renderer->has_staged_scene()) {
                        renderer->discard_staged_scene();
                        staged_index = -1;
                    }
                    preloader.start(target_path);
                }
            }
        } else if (playlist_size > 1 && staged_index < 0) {
            // Idle: prefetch the next scene and stage it as soon as it lands,
            // so the next ] is an immediate descriptor swap.
            if (preloader.ready()) {
                const long loaded_index = scene_index_of(preloader.path());
                if (loaded_index >= 0) {
                    auto loaded = preloader.take();
                    if (loaded.ok()) {
                        const double stage_start = now_ms();
                        const splat::Status staged = renderer->stage_scene(loaded.value().scene);
                        if (staged.ok()) {
                            staged_index = loaded_index;
                            std::printf("[viewer] prefetched %s: preload %.1f ms  stage %.1f ms  "
                                        "staged %.1f MiB\n",
                                        playlist->entry(static_cast<size_t>(loaded_index))
                                            .folder.filename().string().c_str(),
                                        preloader.last_load_ms(), now_ms() - stage_start,
                                        static_cast<double>(renderer->staged_bytes()) /
                                            (1024.0 * 1024.0));
                            std::fflush(stdout);
                        } else {
                            std::fprintf(stderr, "splat_desktop: %s\n", staged.message().c_str());
                        }
                    }
                } else {
                    preloader.cancel();
                }
            } else if (preloader.failed()) {
                std::fprintf(stderr, "splat_desktop: preload failed: %s\n",
                             preloader.error().c_str());
                failed_path = preloader.path();
                preloader.cancel();
            } else if (!preloader.running()) {
                const splat::Path next_path =
                    scene_path_at((static_cast<long>(display_index) + 1) %
                                  static_cast<long>(playlist_size));
                if (next_path != failed_path) preloader.start(next_path);
            }
        }

        view = state.frame(head.position, head.yaw, head.pitch, options.ipd_mm * 0.001f);

        vkResetFences(viewer.context->device(), 1, &viewer.acquire_fence);
        uint32_t image_index = 0;
        VkResult result = vkAcquireNextImageKHR(viewer.context->device(), viewer.swapchain,
                                                UINT64_MAX, VK_NULL_HANDLE,
                                                viewer.acquire_fence, &image_index);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            error = vk_error("vkAcquireNextImageKHR", result);
            ok = false;
            break;
        }
        result = vkWaitForFences(viewer.context->device(), 1, &viewer.acquire_fence, VK_TRUE,
                                 UINT64_MAX);
        if (result != VK_SUCCESS) {
            error = vk_error("vkWaitForFences", result);
            ok = false;
            break;
        }

        splat::RenderTarget target;
        target.image = viewer.images[image_index];
        target.view = viewer.views[image_index];
        target.extent = viewer.extent;
        target.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        target.final_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        const splat::Status status = renderer->render(view, target, false);
        if (!status.ok()) {
            error = status.message();
            ok = false;
            break;
        }

        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.swapchainCount = 1;
        present.pSwapchains = &viewer.swapchain;
        present.pImageIndices = &image_index;
        result = vkQueuePresentKHR(viewer.context->present_queue(), &present);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            error = vk_error("vkQueuePresentKHR", result);
            ok = false;
            break;
        }
        ++rendered;

        if (now - last_print >= 1.0) {
            std::printf("[render] project %.2f ms  sort %.2f ms  draw %.2f ms  visible %u  "
                        "scale %.2f  comfort %.2f  scene %zu/%zu  %s  fps %.1f\n",
                        renderer->timings().project_ms, renderer->timings().sort_ms,
                        renderer->timings().draw_ms, renderer->last_visible_count(),
                        state.world_scale(), view.comfort_fade,
                        playlist ? display_index + 1 : 0, playlist_size,
                        state.paused() ? "paused" : "playing", dt > 0.0f ? 1.0 / dt : 0.0);
            std::fflush(stdout);
            last_print = now;
        }
    }

    renderer.reset();
    destroy_viewer(viewer);

    if (!ok) {
        std::fprintf(stderr, "splat_desktop: %s\n", error.c_str());
        return 1;
    }
    std::printf("splat_desktop: rendered %d frame%s\n", rendered, rendered == 1 ? "" : "s");
    return 0;
}

// App mode (D1) --------------------------------------------------------------
//
// The session (platform/desktop/app_mode.*) owns the LAN page, the inbox
// watcher, the queue, the runner and the GPU phase guard. This file owns the
// GPU objects: it releases them when the session asks for the GPU, reallocates
// them after the runner reported the model unloaded, and swaps scenes through
// the renderer's staging slot so browsing never re-uploads during a frame.

// Device-local heap usage. VK_EXT_memory_budget's usage/budget pair is filled
// in when the driver supports it (MoltenVK does); otherwise the heap size is
// all that can be measured and the log says so.
std::string gpu_memory_report(VkPhysicalDevice device) {
    if (device == VK_NULL_HANDLE) return "n/a";
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 properties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    properties.pNext = &budget;
    vkGetPhysicalDeviceMemoryProperties2(device, &properties);

    uint64_t used = 0;
    uint64_t total = 0;
    bool have_usage = false;
    for (uint32_t heap = 0; heap < properties.memoryProperties.memoryHeapCount; ++heap) {
        const VkMemoryHeap& info = properties.memoryProperties.memoryHeaps[heap];
        if ((info.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0) continue;
        total += info.size;
        if (heap < VK_MAX_MEMORY_HEAPS) {
            used += budget.heapUsage[heap];
            if (budget.heapUsage[heap] != 0) have_usage = true;
        }
    }
    if (total == 0) return "n/a";
    if (!have_usage) return "heap=" + splat::human_bytes(total) + " (usage unknown)";
    return splat::human_bytes(used) + "/" + splat::human_bytes(total);
}

// Neutral backdrop for the frames where no scene is allocated (a conversion
// owns the GPU, or no scene exists yet). sRGB target, so 0.46 encoded is the
// renderer's 0.18 linear backdrop.
void clear_present_image(splat::VulkanContext& context, VkImage image, VkExtent2D extent) {
    (void)extent;
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = context.command_pool();
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(context.device(), &allocate, &command) != VK_SUCCESS) return;

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(command, &begin);

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkClearColorValue color{};
    color.float32[0] = 0.46f;
    color.float32[1] = 0.46f;
    color.float32[2] = 0.46f;
    color.float32[3] = 1.0f;
    vkCmdClearColorImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
                         &barrier.subresourceRange);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
    vkEndCommandBuffer(command);

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    vkQueueSubmit(context.queue(), 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(context.queue());
    vkFreeCommandBuffers(context.device(), context.command_pool(), 1, &command);
}

// The same upload a phone does: PIN login, session cookie, multipart photo.
// Used by --upload so a manual run needs one foreground command.
splat::Result<std::string> upload_photo(const splat::AppMode& app, const std::string& photo) {
    using Outcome = splat::Result<std::string>;
    std::ifstream file(photo, std::ios::binary);
    if (!file) return Outcome::failure("cannot open " + photo);
    const std::string body((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    if (body.empty()) return Outcome::failure("empty file " + photo);

    httplib::Client client(app.bind_address(), app.port());
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(30, 0);
    const auto login =
        client.Post("/api/login", "pin=" + app.pin(), "application/x-www-form-urlencoded");
    if (!login) return Outcome::failure("cannot reach " + app.upload_url());
    if (login->status != 200) {
        return Outcome::failure("PIN rejected (HTTP " + std::to_string(login->status) + ")");
    }
    std::string cookie = login->get_header_value("Set-Cookie");
    const size_t end = cookie.find(';');
    if (end != std::string::npos) cookie.resize(end);
    if (cookie.empty()) return Outcome::failure("login returned no session cookie");

    httplib::MultipartFormDataItems items;
    items.push_back({"photo", body, std::filesystem::path(photo).filename().string(),
                     "image/jpeg"});
    client.set_default_headers({{"Cookie", cookie}});
    const auto response = client.Post("/api/upload", items);
    if (!response) return Outcome::failure("upload request failed");
    if (response->status != 200) {
        return Outcome::failure("upload rejected (HTTP " + std::to_string(response->status) +
                                "): " + response->body);
    }
    try {
        const nlohmann::json parsed = nlohmann::json::parse(response->body);
        if (parsed.value("ok", false)) return Outcome::success(parsed.value("file", std::string()));
    } catch (const std::exception&) {
    }
    return Outcome::failure("unexpected upload response: " + response->body);
}

// One offscreen frame of a scene, written as a PNG. App mode calls it for each
// new scene when --screenshot-dir is set, which is how the manual run records
// what the viewer shows without a window.
bool render_screenshot(splat::VulkanContext& device, const Options& options,
                       const splat::LoadedScene& load, const std::string& out_path) {
    uint32_t eye_width = options.width;
    uint32_t eye_height = options.height;
    if (eye_width == 0 || eye_height == 0) {
        if (intrinsics_valid(load.meta.intrinsics)) {
            eye_width = load.meta.intrinsics.width;
            eye_height = load.meta.intrinsics.height;
        } else {
            eye_width = 1280;
            eye_height = 720;
        }
    }

    const VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;
    auto renderer = make_renderer(device, options, eye_width, eye_height, load.meta.intrinsics,
                                  format, load.scene, false);
    if (!renderer) return false;

    const VkExtent2D extent = renderer->target_extent();
    auto image = splat::create_vulkan_color_image(device, format, extent,
                                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!image.ok()) {
        std::fprintf(stderr, "splat_desktop: %s\n", image.message().c_str());
        return false;
    }

    splat::ViewerState state;
    state.set_world_scale(options.world_scale);
    state.set_comfort_radius(options.comfort_radius);
    state.set_fade_start(options.fade_start);
    const float head[3] = {options.pose.position[0], options.pose.position[1],
                           options.pose.position[2]};
    const splat::FrameView view =
        state.frame(head, options.pose.yaw, options.pose.pitch, options.ipd_mm * 0.001f);

    splat::RenderTarget target;
    target.image = image.value().image;
    target.view = image.value().view;
    target.extent = extent;
    target.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    target.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    const splat::Status rendered = renderer->render(view, target, true);
    if (!rendered.ok()) {
        std::fprintf(stderr, "splat_desktop: %s\n", rendered.message().c_str());
        splat::destroy_vulkan_image(device, image.value());
        return false;
    }
    auto pixels = renderer->read_image();
    if (!pixels.ok()) {
        std::fprintf(stderr, "splat_desktop: %s\n", pixels.message().c_str());
        splat::destroy_vulkan_image(device, image.value());
        return false;
    }
    const splat::Image& output = pixels.value();
    const bool written =
        stbi_write_png(out_path.c_str(), output.width, output.height, 3, output.rgb.data(),
                       output.width * 3) != 0;
    std::printf("[app] screenshot %s %dx%d visible %u splats %u\n", out_path.c_str(),
                output.width, output.height, renderer->last_visible_count(),
                renderer->scene_count());
    splat::destroy_vulkan_image(device, image.value());
    return written;
}

// Window, instance, surface, device and swapchain. Kept together so the loop
// below only deals with scenes and phases.
bool init_app_viewer(Viewer& viewer, const Options& options, std::string& error) {
    glfwSetErrorCallback(glfw_error);
    if (!glfwInit()) {
        error = "glfwInit failed";
        return false;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    uint32_t width = options.width == 0 ? 1280 : options.width;
    uint32_t height = options.height == 0 ? 720 : options.height;
    if (options.stereo) width *= 2;
    viewer.window = glfwCreateWindow(static_cast<int>(width), static_cast<int>(height),
                                     "Photo Splat VR (app mode)", nullptr, nullptr);
    if (viewer.window == nullptr) {
        error = "glfwCreateWindow failed";
        return false;
    }
    glfwSetScrollCallback(viewer.window, scroll_callback);
    glfwSetInputMode(viewer.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    if (!create_instance(viewer, error)) return false;
    if (glfwCreateWindowSurface(viewer.instance, viewer.window, nullptr, &viewer.surface) !=
        VK_SUCCESS) {
        error = "glfwCreateWindowSurface failed";
        return false;
    }
    splat::VulkanContextOptions context_options;
    context_options.instance = viewer.instance;
    context_options.surface = viewer.surface;
    context_options.present = true;
    auto context = splat::VulkanContext::create(context_options);
    if (!context.ok()) {
        error = context.message();
        return false;
    }
    viewer.context = context.take();

    int framebuffer_width = 0;
    int framebuffer_height = 0;
    glfwGetFramebufferSize(viewer.window, &framebuffer_width, &framebuffer_height);
    if (!create_swapchain(viewer, static_cast<uint32_t>(framebuffer_width),
                          static_cast<uint32_t>(framebuffer_height), error)) {
        return false;
    }
    if (!create_viewer_sync(viewer, error)) return false;

    std::printf("splat_desktop: %s, Vulkan %u.%u, swapchain %ux%u\n",
                viewer.context->properties().deviceName,
                VK_VERSION_MAJOR(viewer.context->properties().apiVersion),
                VK_VERSION_MINOR(viewer.context->properties().apiVersion), viewer.extent.width,
                viewer.extent.height);
    std::printf("controls: mouse look, WASD move, [ ] previous/next scene, scroll or +/- "
                "scale, R recentre, P pause, ESC quit\n");
    return true;
}

void destroy_viewer(Viewer& viewer) {
    if (viewer.context) vkDeviceWaitIdle(viewer.context->device());
    for (VkImageView view_handle : viewer.views) {
        if (view_handle != VK_NULL_HANDLE) {
            vkDestroyImageView(viewer.context->device(), view_handle, nullptr);
        }
    }
    viewer.views.clear();
    if (viewer.acquire_fence != VK_NULL_HANDLE) {
        vkDestroyFence(viewer.context->device(), viewer.acquire_fence, nullptr);
        viewer.acquire_fence = VK_NULL_HANDLE;
    }
    if (viewer.swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(viewer.context->device(), viewer.swapchain, nullptr);
        viewer.swapchain = VK_NULL_HANDLE;
    }
    viewer.context.reset();
    if (viewer.surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(viewer.instance, viewer.surface, nullptr);
        viewer.surface = VK_NULL_HANDLE;
    }
    if (viewer.instance != VK_NULL_HANDLE) {
        vkDestroyInstance(viewer.instance, nullptr);
        viewer.instance = VK_NULL_HANDLE;
    }
    if (viewer.window != nullptr) {
        glfwDestroyWindow(viewer.window);
        viewer.window = nullptr;
        glfwTerminate();
    }
}

int run_app_window(splat::AppMode& app, const Options& options, Viewer& viewer) {
    const uint32_t eye_width = viewer.extent.width / (options.stereo ? 2u : 1u);
    const uint32_t eye_height = viewer.extent.height;

    splat::ViewerState state;
    state.set_world_scale(options.world_scale);
    state.set_comfort_radius(options.comfort_radius);
    state.set_fade_start(options.fade_start);

    HeadState head;
    head.position[0] = options.pose.position[0];
    head.position[1] = options.pose.position[1];
    head.position[2] = options.pose.position[2];
    head.yaw = options.pose.yaw;
    head.pitch = options.pose.pitch;

    std::unique_ptr<splat::SplatRenderer> renderer;
    splat::ScenePreloader preloader;
    bool holding = false;
    bool displayed = false;
    size_t displayed_index = 0;
    uint64_t displayed_generation = 0;
    // A scene that failed to load is remembered so the viewer does not retry
    // (and print the same error) every frame until the list changes.
    splat::Path failed_scene;
    uint64_t failed_generation = 0;
    std::string error;

    auto release_gpu = [&]() {
        if (!holding) return;
        // Free the scene buffers (and the sort/pipeline allocations) before the
        // runner's backend loads, then tell the guard the viewer is out.
        renderer.reset();
        preloader.cancel();
        app.end_viewing();
        holding = false;
        app.log_transition("app.viewer.released", "scene buffers freed");
    };

    auto show_loaded = [&](const splat::LoadedScene& load) -> bool {
        renderer = make_renderer(*viewer.context, options, eye_width, eye_height,
                                 load.meta.intrinsics, viewer.format, load.scene, true);
        if (!renderer) return false;
        displayed = true;
        displayed_index = app.current_index();
        displayed_generation = app.scene_generation();
        const auto entry = app.current_scene();
        app.log_transition("app.viewer.allocated",
                           "scene=" + (entry ? entry->folder.filename().string() : std::string()) +
                               " splats=" + std::to_string(load.scene.count()));
        return true;
    };

    // Swap the displayed scene through the renderer's staging slot: the old
    // scene keeps rendering until the commit, and no pipeline is rebuilt.
    auto swap_scene = [&](const splat::PlaylistEntry& target) -> bool {
        std::optional<splat::LoadedScene> loaded;
        if (preloader.ready() && preloader.path() == target.scene_file) {
            auto taken = preloader.take();
            if (taken.ok()) loaded = std::move(taken.value());
        } else {
            preloader.cancel();
            auto read = splat::read_scene_path(target.scene_file);
            if (read.ok()) loaded = std::move(read.value());
            else std::fprintf(stderr, "splat_desktop: %s\n", read.message().c_str());
        }
        if (!loaded) return false;
        if (renderer->has_staged_scene()) renderer->discard_staged_scene();
        const splat::Status staged = renderer->stage_scene(loaded->scene);
        if (!staged.ok()) {
            std::fprintf(stderr, "splat_desktop: %s\n", staged.message().c_str());
            return false;
        }
        const splat::Status committed = renderer->commit_staged_scene();
        if (!committed.ok()) {
            std::fprintf(stderr, "splat_desktop: %s\n", committed.message().c_str());
            return false;
        }
        displayed = true;
        displayed_index = app.current_index();
        displayed_generation = app.scene_generation();
        app.log_transition("app.viewer.scene",
                           "scene=" + target.folder.filename().string() +
                               " splats=" + std::to_string(loaded->scene.count()) +
                               " commit_ms=" + std::to_string(renderer->last_commit_ms()));
        return true;
    };

    bool key_down[GLFW_KEY_LAST + 1] = {};
    auto pressed = [&](int key) {
        const bool down = glfwGetKey(viewer.window, key) == GLFW_PRESS;
        const bool edge = down && !key_down[key];
        key_down[key] = down;
        return edge;
    };

    splat::FrameView view;
    view.ipd = options.ipd_mm * 0.001f;

    int rendered = 0;
    double last_time = glfwGetTime();
    double last_print = last_time;
    bool ok = true;
    while (ok) {
        glfwPollEvents();
        // Ctrl-C / SIGTERM: leave here and let run_app() stop the session in
        // order (page, worker/model unload, renderer).
        if (g_stop_requested.load()) {
            log_stop_requested(app);
            break;
        }
        if (glfwGetKey(viewer.window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
        if (glfwWindowShouldClose(viewer.window) != 0) break;
        if (options.smoke_frames >= 0 && rendered >= options.smoke_frames) break;

        const double now = glfwGetTime();
        const float dt = static_cast<float>(now - last_time);
        last_time = now;

        if (pressed(GLFW_KEY_P)) state.set_paused(!state.paused());

        if (!state.paused()) {
            update_head(head, viewer.window, dt);
            if (pressed(GLFW_KEY_R)) {
                state.recentre(head.position, head.yaw);
                app.log_transition("app.viewer.recentre",
                                   "anchor=" + std::to_string(state.anchor()[0]) + "," +
                                       std::to_string(state.anchor()[1]) + "," +
                                       std::to_string(state.anchor()[2]));
            }
            if (app.scene_count() > 0 && pressed(GLFW_KEY_RIGHT_BRACKET)) app.next();
            if (app.scene_count() > 0 && pressed(GLFW_KEY_LEFT_BRACKET)) app.previous();
            if (pressed(GLFW_KEY_EQUAL) || pressed(GLFW_KEY_KP_ADD)) state.scale_world(1.1f);
            if (pressed(GLFW_KEY_MINUS) || pressed(GLFW_KEY_KP_SUBTRACT)) {
                state.scale_world(1.0f / 1.1f);
            }
            if (std::abs(g_scroll) > 1.0e-6) {
                state.scale_world(std::exp(static_cast<float>(g_scroll) * 0.1f));
                g_scroll = 0.0;
            }
        }

        // GPU exclusivity: out before inference, back in only once the runner
        // reported the model unloaded (the guard enforces the second half).
        if (holding && (app.release_requested() || app.work_pending())) release_gpu();

        if (!holding && !app.work_pending() && app.scene_count() > 0) {
            const auto entry = app.current_scene();
            const bool known_bad =
                entry && entry->scene_file == failed_scene &&
                app.scene_generation() == failed_generation;
            if (entry && !known_bad && app.try_begin_viewing()) {
                holding = true;
                auto load = splat::read_scene_path(entry->scene_file);
                if (!load.ok()) {
                    std::fprintf(stderr, "splat_desktop: %s\n", load.message().c_str());
                    failed_scene = entry->scene_file;
                    failed_generation = app.scene_generation();
                    release_gpu();
                } else if (!show_loaded(load.value())) {
                    error = "cannot create the splat renderer";
                    ok = false;
                    break;
                }
            }
        }

        // A new scene (finished job) or a browse step while holding the GPU.
        if (holding && displayed &&
            (app.current_index() != displayed_index ||
             app.scene_generation() != displayed_generation)) {
            const auto entry = app.current_scene();
            if (entry) {
                if (!swap_scene(*entry)) {
                    // Fall back to a full reallocation: drop to the backdrop and
                    // let the allocation branch above rebuild the renderer.
                    release_gpu();
                }
            }
        }

        // Idle prefetch: the next browse entry is read while this one renders.
        if (holding && displayed && app.scene_count() > 1 && !preloader.running() &&
            !preloader.ready()) {
            const auto next_entry = app.scene((displayed_index + 1) % app.scene_count());
            const auto shown = app.current_scene();
            if (next_entry && shown && next_entry->scene_file != shown->scene_file) {
                preloader.start(next_entry->scene_file);
            }
        }

        view = state.frame(head.position, head.yaw, head.pitch, options.ipd_mm * 0.001f);

        vkResetFences(viewer.context->device(), 1, &viewer.acquire_fence);
        uint32_t image_index = 0;
        VkResult result = vkAcquireNextImageKHR(viewer.context->device(), viewer.swapchain,
                                                UINT64_MAX, VK_NULL_HANDLE, viewer.acquire_fence,
                                                &image_index);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            error = vk_error("vkAcquireNextImageKHR", result);
            ok = false;
            break;
        }
        result = vkWaitForFences(viewer.context->device(), 1, &viewer.acquire_fence, VK_TRUE,
                                 UINT64_MAX);
        if (result != VK_SUCCESS) {
            error = vk_error("vkWaitForFences", result);
            ok = false;
            break;
        }

        splat::RenderTarget target;
        target.image = viewer.images[image_index];
        target.view = viewer.views[image_index];
        target.extent = viewer.extent;
        target.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        target.final_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        if (renderer) {
            const splat::Status status = renderer->render(view, target, false);
            if (!status.ok()) {
                error = status.message();
                ok = false;
                break;
            }
        } else {
            clear_present_image(*viewer.context, viewer.images[image_index], viewer.extent);
        }

        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.swapchainCount = 1;
        present.pSwapchains = &viewer.swapchain;
        present.pImageIndices = &image_index;
        result = vkQueuePresentKHR(viewer.context->present_queue(), &present);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            error = vk_error("vkQueuePresentKHR", result);
            ok = false;
            break;
        }
        ++rendered;

        if (now - last_print >= 2.0) {
            const auto entry = app.current_scene();
            std::printf("[app] %s  scene %zu/%zu %s  %s  %s  fps %.1f\n",
                        app.backend().c_str(), app.current_index() + 1, app.scene_count(),
                        entry ? entry->folder.filename().string().c_str() : "-",
                        holding ? "viewing" : "released",
                        app.work_pending() ? "converting" : "idle",
                        dt > 0.0f ? 1.0 / dt : 0.0);
            std::fflush(stdout);
            last_print = now;
        }

        // Scripted exits come last, so the frame that shows a freshly converted
        // scene is rendered before the loop leaves.
        if (options.exit_after_jobs >= 0 &&
            app.finished_jobs() >= static_cast<size_t>(options.exit_after_jobs) &&
            !app.work_pending() && displayed &&
            app.scene_generation() == displayed_generation) {
            break;
        }
        if (options.run_seconds > 0 && app.seconds_since_start() >= options.run_seconds) break;
    }

    release_gpu();
    if (!ok) {
        std::fprintf(stderr, "splat_desktop: %s\n", error.c_str());
        return 1;
    }
    std::printf("splat_desktop: app mode rendered %d frame%s\n", rendered,
                rendered == 1 ? "" : "s");
    return 0;
}

int run_app_headless(splat::AppMode& app, const Options& options, splat::VulkanContext* device) {
    const bool want_screenshots = device != nullptr && !options.screenshot_dir.empty();
    if (want_screenshots) {
        std::error_code error;
        std::filesystem::create_directories(options.screenshot_dir, error);
    }
    std::printf("[app] headless%s\n", want_screenshots ? " with screenshots" : "");
    std::fflush(stdout);

    bool holding = false;
    std::set<std::string> shot;

    while (true) {
        if (g_stop_requested.load()) {
            log_stop_requested(app);
            break;
        }
        if (app.work_pending()) {
            if (holding) {
                app.end_viewing();
                holding = false;
                app.log_transition("app.viewer.released", "headless screenshot loop");
            }
        } else if (want_screenshots && app.scene_count() > 0 &&
                   shot.size() < app.scene_count()) {
            // One scene per tick: every scene that joined the playlist gets its
            // own screenshot, even when two conversions finish back to back.
            for (size_t index = 0; index < app.scene_count(); ++index) {
                const auto entry = app.scene(index);
                if (!entry) continue;
                const std::string folder = entry->folder.filename().string();
                if (shot.count(folder) != 0) continue;
                if (!holding) {
                    if (!app.try_begin_viewing()) break;
                    holding = true;
                }
                auto loaded = splat::read_scene_path(entry->scene_file);
                if (!loaded.ok()) {
                    std::fprintf(stderr, "splat_desktop: %s\n", loaded.message().c_str());
                    shot.insert(folder);
                    break;
                }
                app.log_transition("app.viewer.allocated",
                                   "scene=" + folder +
                                       " splats=" + std::to_string(loaded.value().scene.count()));
                const std::string path =
                    (std::filesystem::path(options.screenshot_dir) / (folder + ".png")).string();
                if (render_screenshot(*device, options, loaded.value(), path)) shot.insert(folder);
                break;
            }
        }

        const bool jobs_done =
            options.exit_after_jobs >= 0 &&
            app.finished_jobs() >= static_cast<size_t>(options.exit_after_jobs);
        if (jobs_done && !app.work_pending()) {
            const bool screenshots_done = !want_screenshots || shot.size() >= app.scene_count();
            if (screenshots_done) break;
        }
        if (options.run_seconds > 0 && app.seconds_since_start() >= options.run_seconds) break;

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (holding) app.end_viewing();
    return 0;
}

int run_app(const Options& options) {
    // Ctrl-C / SIGTERM request a clean stop; the loops below poll the flag. The
    // handler itself only stores it (see handle_stop_signal).
    std::signal(SIGINT, handle_stop_signal);
    std::signal(SIGTERM, handle_stop_signal);

    Viewer viewer;
    std::string error;
    std::unique_ptr<splat::VulkanContext> headless_device;
    bool have_window = false;

    if (!options.headless) {
        if (!init_app_viewer(viewer, options, error)) {
            std::fprintf(stderr,
                         "splat_desktop: %s; running app mode without a window\n",
                         error.c_str());
            destroy_viewer(viewer);
        } else {
            have_window = true;
        }
    }
    if (!have_window && !options.screenshot_dir.empty()) {
        splat::VulkanContextOptions context_options;
        auto context = splat::VulkanContext::create(context_options);
        if (!context.ok()) {
            std::fprintf(stderr, "splat_desktop: no GPU for screenshots: %s\n",
                         context.message().c_str());
        } else {
            headless_device = context.take();
            std::printf("splat_desktop: %s, Vulkan %u.%u (headless)\n",
                        headless_device->properties().deviceName,
                        VK_VERSION_MAJOR(headless_device->properties().apiVersion),
                        VK_VERSION_MINOR(headless_device->properties().apiVersion));
        }
    }

    VkPhysicalDevice physical = VK_NULL_HANDLE;
    if (have_window) {
        physical = viewer.context->physical_device();
    } else if (headless_device) {
        physical = headless_device->physical_device();
    }

    splat::AppModeOptions app_options;
    app_options.data_dir = splat::Path(options.app_data_dir);
    app_options.backend = options.app_backend;
    if (!options.app_model.empty()) app_options.model_path = splat::Path(options.app_model);
    app_options.bind_address = options.app_bind;
    app_options.port = options.port;
    app_options.transition_log = options.transition_log.empty()
                                     ? splat::Path()
                                     : splat::Path(options.transition_log);
    app_options.prefer_vulkan = !options.cpu;
    if (physical != VK_NULL_HANDLE) {
        app_options.gpu_memory = [physical] { return gpu_memory_report(physical); };
    }

    splat::AppMode app(std::move(app_options));
    const splat::Status started = app.start();
    if (!started.ok()) {
        std::fprintf(stderr, "splat_desktop: %s\n", started.message().c_str());
        if (have_window) destroy_viewer(viewer);
        return 1;
    }
    if (have_window) {
        glfwSetWindowTitle(viewer.window, app.window_title().c_str());
    }

    int upload_failures = 0;
    for (const std::string& photo : options.uploads) {
        auto stored = upload_photo(app, photo);
        if (!stored.ok()) {
            std::fprintf(stderr, "splat_desktop: upload %s failed: %s\n", photo.c_str(),
                         stored.message().c_str());
            ++upload_failures;
        } else {
            std::printf("[app] uploaded %s as %s\n", photo.c_str(), stored.value().c_str());
            std::fflush(stdout);
        }
    }
    if (!options.uploads.empty() &&
        upload_failures == static_cast<int>(options.uploads.size())) {
        std::fprintf(stderr,
                     "splat_desktop: no upload reached the page; nothing to convert\n");
        app.stop();
        if (have_window) destroy_viewer(viewer);
        headless_device.reset();
        return 1;
    }

    int code = 0;
    if (have_window) {
        code = run_app_window(app, options, viewer);
    } else {
        code = run_app_headless(app, options, headless_device.get());
    }

    // The transition log's GPU probe reads the device, so stop the session
    // before the Vulkan objects go away.
    app.stop();
    if (have_window) destroy_viewer(viewer);
    headless_device.reset();

    std::printf("[app] done: %zu conversion%s, log %s\n", app.finished_jobs(),
                app.finished_jobs() == 1 ? "" : "s", app.transition_log_path().string().c_str());
    return code;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage(argv[0]);
        return 2;
    }

    if (!options.app_data_dir.empty()) return run_app(options);

    const double start_ms = now_ms();
    std::unique_ptr<splat::ScenePlaylist> playlist;
    std::string scene_path = options.scene_path;
    if (!options.scenes_dir.empty()) {
        auto opened = splat::ScenePlaylist::from_folder(splat::Path(options.scenes_dir));
        if (!opened.ok()) {
            std::fprintf(stderr, "splat_desktop: %s\n", opened.message().c_str());
            return 1;
        }
        playlist = std::make_unique<splat::ScenePlaylist>(opened.take());
        scene_path = playlist->current().scene_file.string();
    }

    auto load = load_scene(scene_path);
    if (!load.ok()) {
        std::fprintf(stderr, "splat_desktop: %s\n", load.message().c_str());
        return 1;
    }
    const double scene_read_ms = now_ms() - start_ms;
    std::printf("splat_desktop: %s, %zu Gaussians, scene read %.1f ms\n", scene_path.c_str(),
                load.value().scene.count(), scene_read_ms);
    if (playlist) {
        std::printf("splat_desktop: playlist %zu scene%s\n", playlist->size(),
                    playlist->size() == 1 ? "" : "s");
    }

    if (options.playlist_loops > 0 && !playlist) {
        std::fprintf(stderr, "splat_desktop: --playlist needs --scenes\n");
        return 2;
    }
    if (!options.screenshot.empty() || options.bench_frames > 0 || options.playlist_loops > 0) {
        return run_headless(options, load.value(), playlist.get(), start_ms);
    }
    return run_viewer(options, load.value(), playlist.get());
}
