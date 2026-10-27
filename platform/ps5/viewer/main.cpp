// The viewer title: a .psplat scene rendered by the project's own renderer
// (core/render: SplatRenderer, ViewerState, comfort fade) and presented on the
// TV through VideoOut — mono, at the TV's resolution.
//
// This is milestone V1, "splats on the TV": the first thing the project shows
// on the console. It is a homebrew title, not a payload, because a title is the
// process that gets the direct-memory pool RADV turns into its device heap
// (docs/research/spike-b-memory.md sections 1-2); as a payload RADV would have
// zero heaps and nothing to draw into.
//
// What one run does:
//
//   1. read <dataDir>/bench/viewer.env (bench.env-style key=value: the scene,
//      the run length, the output size, driver settings), because a title
//      launched through ProsperoMgr gets no argv;
//   2. log the direct and flexible pools, then bring RADV up the way PS5LM does
//      (radv_enable_unified_heap_on_apu, stderr to klog);
//   3. find the scene folders under <dataDir>/scenes/ and load one
//      (SCENE=<folder>, default the first by name) with core/scene;
//   4. open VideoOut at 1920x1080 (the TV's size) with two direct-memory
//      framebuffers (platform/ps5/viewer/videoout.cpp);
//   5. create the C6 renderer for a mono 1920x1080 target and render from the
//      capture pose with a slow side-to-side sway inside 60 % of the comfort
//      radius, so parallax is visible; each frame is copied out of the RADV
//      image and written into the VideoOut tile layout, then flipped;
//   6. log the per-pass GPU times and the frame rate once a second;
//   7. end after SECONDS (or on <dataDir>/title-quit) with
//      sceSystemServiceLoadExec("exit", nullptr) — a title never exit()s.
//
// <dataDir> is the data folder of the identity the package was built as
// (identities.json: dev or the user's release folder), compiled in by
// build-title.mk as SPLAT_DATA_ROOT and spelled here only through data_path().
//
// Bounded waits, leak-on-timeout: every GPU wait has a timeout. If one fails
// (a device lost or a wedged submission), the renderer, its buffers and the
// device are deliberately NOT destroyed — nothing is freed under a GPU that may
// still be reading it — the log says so and the title ends. That is vk_hello's
// teardown policy (docs/research/spike-b.md section 4).
//
// Build: make -C platform/ps5/viewer            -> build/ps5/title/PPSA99202/
// Run:   platform/ps5/scripts/run-viewer.sh     (see docs/console-runbook.md 1b)
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>

#include <ps5platform/kernel.h>
#include <ps5platform/klog.h>

// The SDK's <sys/sysctl.h> needs u_int from <sys/types.h>; Vulkan's own system
// includes come after it.
#include <vulkan/vulkan.h>

#include "common/path.h"
#include "common/progress.h"
#include "gallery/gallery.h"
#include "gallery/samples.h"
#include "jobs/image_io.h"
#include "render/scene_playlist.h"
#include "render/splat_renderer.h"
#include "render/viewer_state.h"
#include "render/vulkan_context.h"
#include "scene/scene_folder.h"
#include "ui/app.h"
#include "ui/input_script.h"
#include "ui/pad_bits.h"

// The W18 convert spike's second Vulkan user: ggml-vulkan's own device and the
// conversion chain W4 linked into this title. platform/ps5/ggml is on the
// include path ($(GLUE) in the Makefile) for the ggml log bridge and the
// peak-RSS helper the benches log with.
#include "bench_env.h"
#include "convert_gate.h"
#include "convert_session.h"
#include "eye_buffers.h"
#include "ggml-backend.h"
#include "ggml-vulkan.h"
#include "ggml_logging.h"
#include "pad_input.h"
#include "scene_switch.h"
#include "title_common.h"
#include "ui_host.h"
#include "ui_setup.h"
#include "videoout.h"
#include "viewer_env_defaults.h"
#include "vr_mode.h"

extern "C" int sceSystemServiceHideSplashScreen(void);
extern "C" int sceSystemServiceLoadExec(const char* path, const char* const* args);

namespace {

using viewer::line;
using viewer::now_ms;
using viewer::run_with_big_stack;
// viewer.env's keys, the panel and the one data root are shared with the VR
// path (ui_setup.h): both modes build the panel through the same call.
using viewer::data_path;
using viewer::kDataDir;
using viewer::Options;

double gib(int64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
}

// The two pools a title process has, read not assumed. Direct memory is what
// RADV's PS5 winsys turns into its one device heap; flexible memory is libc's.
void log_pools() {
    const int64_t direct = sceKernelGetDirectMemorySize();
    size_t available = 0;
    int64_t physical = 0;
    const int32_t rv = sceKernelAvailableDirectMemorySize(0, direct, 2 * 1024 * 1024, &physical,
                                                          &available);
    line("viewer: direct pool %lld bytes (%.2f GiB), available %zu bytes (%.2f GiB) rv=0x%08x",
         static_cast<long long>(direct), gib(direct), available, gib(static_cast<int64_t>(available)),
         static_cast<uint32_t>(rv));

    size_t configured = 0;
    size_t free_flexible = 0;
    const int32_t rv_configured = sceKernelConfiguredFlexibleMemorySize(&configured);
    const int32_t rv_free = sceKernelAvailableFlexibleMemorySize(&free_flexible);
    line("viewer: flexible pool %zu MiB configured rv=0x%08x, %zu MiB free rv=0x%08x",
         configured / (1024 * 1024), static_cast<uint32_t>(rv_configured),
         free_flexible / (1024 * 1024), static_cast<uint32_t>(rv_free));
}

// ---- configuration ---------------------------------------------------------
// bench.env style: `KEY=value`, `#` comments, blank lines. Every entry is also
// exported (setenv), before the first GPU call, so a driver setting such as
// RADV_DEBUG or MESA_SHADER_CACHE_DISABLE works through the same file the
// benches use.
const std::string kEnvPath = viewer::data_path("bench/viewer.env");
const std::string kQuitPath = viewer::data_path("title-quit");
constexpr const char* kAppShaderDir = "/app0/shaders";
const std::string kDataShaderDir = viewer::data_path("shaders");

std::string trim(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string read_text_file(const std::string& path) {
    std::string text;
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return text;
    char buffer[4096];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, read);
    std::fclose(file);
    return text;
}

// mkdir -p for the shots directory: every component that is missing is created,
// an existing non-directory is an error. An absolute path starts at "/"; a
// relative one stays relative to the working directory (a leading "/" added to
// each component would turn "shots" into the read-only "/shots").
bool ensure_directory(const std::string& path) {
    if (path.empty()) return false;
    std::string current = path.front() == '/' ? std::string() : std::string(".");
    size_t pos = 0;
    while (pos <= path.size()) {
        const size_t slash = path.find('/', pos);
        const std::string part =
            path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (!part.empty() && part != ".") {
            current += "/" + part;
            struct stat st {};
            if (::stat(current.c_str(), &st) == 0) {
                if (!S_ISDIR(st.st_mode)) {
                    errno = ENOTDIR;
                    return false;
                }
            } else if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
                return false;
            }
        }
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return true;
}

// viewer.env may name SHOTS_DIR relative to the console data root
// (`SHOTS_DIR=shots`): that means /data/ps5-splat-vr/shots, the mount the
// title may write. An absolute path is used as given.
std::string resolve_data_path(const std::string& path) {
    if (path.empty() || path.front() == '/') return path;
    return std::string(kDataDir) + "/" + path;
}

// Loads the file, exports each entry, and returns the raw text so the caller
// can read its own keys before the environment is trusted.
std::string load_env_file(const std::string& path) {
    const std::string text = read_text_file(path);
    if (text.empty()) return text;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string entry = trim(text.substr(pos, end - pos));
        pos = end + 1;
        if (entry.empty() || entry[0] == '#') continue;
        const size_t equals = entry.find('=');
        if (equals == std::string::npos) continue;
        std::string key = trim(entry.substr(0, equals));
        const std::string value = trim(entry.substr(equals + 1));
        if (key.empty()) continue;
        if (!(std::isalpha(static_cast<unsigned char>(key[0])) || key[0] == '_')) continue;
        bool valid = true;
        for (char character : key) {
            if (!(std::isalnum(static_cast<unsigned char>(character)) || character == '_')) {
                valid = false;
                break;
            }
        }
        if (!valid) continue;
        setenv(key.c_str(), value.c_str(), 1);
    }
    return text;
}

std::string env_value(const std::string& text, const char* key) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string entry = trim(text.substr(pos, end - pos));
        pos = end + 1;
        if (entry.empty() || entry[0] == '#') continue;
        const size_t equals = entry.find('=');
        if (equals == std::string::npos) continue;
        if (trim(entry.substr(0, equals)) != key) continue;
        return trim(entry.substr(equals + 1));
    }
    return {};
}

bool parse_u32(const std::string& text, uint32_t& value) {
    if (text.empty()) return false;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') return false;
    value = static_cast<uint32_t>(parsed);
    return true;
}

bool parse_float(const std::string& text, float& value) {
    if (text.empty()) return false;
    char* end = nullptr;
    const float parsed = std::strtof(text.c_str(), &end);
    if (end == nullptr || *end != '\0') return false;
    value = parsed;
    return true;
}

// Options is ui_setup.h's now (W16): the same struct the VR path fills through
// ui_options_from_env(), so the TV and the panel cannot drift apart.

// ---- T7 spike: PROBE=sources ------------------------------------------------
// Answers "can a title read the console's photo locations?" (task T7,
// docs/research/browse-ui.md section 1): for each source root, opendir's result
// and entry count (entering up to 3 levels below /user/av_contents/photo, 1
// level below a storage volume, 200 entries per root at most), and for the
// first .jpg/.jpeg/.png/.jxr found whether open plus a 16-byte read works —
// only the errno or the 4-byte magic is logged, never file contents. Runs once
// when viewer.env says PROBE=sources, then the viewer continues normally.
//
// The root list carries ShadowMountPlus's per-title alias set
// (docs/research/browse-ui.md section 1.4 option B): the sandbox keeps /mnt
// local and mounts each *mounted* volume at "<sandbox>/mnt/usb0..7",
// "<sandbox>/mnt/ext0..1" and "<sandbox>/mnt/disc" (ShadowMountPlus
// src/sm_fakelib.c mount_sandbox_storage_roots + mount_sandbox_directory,
// README.md:206), so inside the title those aliases appear as /mnt/usb0..7,
// /mnt/ext0..1, /mnt/disc and are reachable again as /app0/../mnt/<same>.
// No new dependencies.
struct SourceProbe {
    std::string first_image;
    int entries = 0;
    int cap = 200;
    int subdir_errno = 0;  // first subfolder opendir that failed
};

bool probe_is_image(const char* name) {
    const char* dot = std::strrchr(name, '.');
    if (dot == nullptr) return false;
    // The inbox pipeline takes jpg/jpeg/png (core/jobs/inbox_watcher.cpp:20-23,
    // browse-ui.md section 1.5); .jxr stays listed because captures use it.
    std::string ext;
    for (const char* c = dot; *c != '\0'; ++c) ext.push_back(static_cast<char>(std::tolower(*c)));
    return ext == ".jxr" || ext == ".png" || ext == ".jpg" || ext == ".jpeg";
}

// Returns this level's opendir errno (0 = ok); depth_left is how many levels
// below `path` may be entered.
int probe_walk(const std::string& path, int depth_left, SourceProbe& probe) {
    DIR* dir = ::opendir(path.c_str());
    if (dir == nullptr) return errno;
    while (probe.entries < probe.cap) {
        const struct dirent* entry = ::readdir(dir);
        if (entry == nullptr) break;
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        ++probe.entries;
        const std::string child = path + "/" + name;
        bool is_dir = entry->d_type == DT_DIR;
        if (entry->d_type == DT_UNKNOWN) {
            struct stat st {};
            is_dir = ::stat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        }
        if (is_dir) {
            if (depth_left > 0) {
                const int err = probe_walk(child, depth_left - 1, probe);
                if (err != 0 && probe.subdir_errno == 0) probe.subdir_errno = err;
            }
        } else if (probe.first_image.empty() && probe_is_image(name.c_str())) {
            probe.first_image = child;
        }
    }
    ::closedir(dir);
    return 0;
}

void probe_root(const std::string& path, int depth) {
    SourceProbe probe;
    const int err = probe_walk(path, depth, probe);
    if (err != 0) {
        line("viewer: probe %s: opendir errno %d (%s)", path.c_str(), err, std::strerror(err));
        return;
    }
    line("viewer: probe %s: opendir ok, %d entries%s%s", path.c_str(), probe.entries,
         probe.entries >= probe.cap ? " (capped)" : "",
         probe.subdir_errno != 0 ? " (a subfolder failed)" : "");
    if (probe.subdir_errno != 0) {
        line("viewer: probe %s: subfolder opendir errno %d", path.c_str(), probe.subdir_errno);
    }
    if (probe.first_image.empty()) return;
    std::FILE* file = std::fopen(probe.first_image.c_str(), "rb");
    if (file == nullptr) {
        line("viewer: probe %s: image open errno %d (%s)", probe.first_image.c_str(), errno,
             std::strerror(errno));
        return;
    }
    unsigned char magic[16] = {};
    const size_t got = std::fread(magic, 1, sizeof(magic), file);
    if (got < 4) {
        line("viewer: probe %s: image read %zu of 16 bytes, errno %d", probe.first_image.c_str(),
             got, errno);
    } else {
        line("viewer: probe %s: read %zu/16 bytes, magic %02x%02x%02x%02x", probe.first_image.c_str(),
             got, magic[0], magic[1], magic[2], magic[3]);
    }
    std::fclose(file);
}

void probe_sources() {
    line("viewer: probe sources (T7 spike, PROBE=sources): listing the console's photo locations");
    // Captures: /user/av_contents (T7: ENOENT inside the title).
    probe_root("/user", 0);
    probe_root("/user/av_contents", 0);
    probe_root("/user/av_contents/photo", 3);
    // The sandbox's own /mnt: ShadowMountPlus only creates an alias for a
    // volume that is actually mounted, so this list is the alias set.
    probe_root("/mnt", 0);
    for (int i = 0; i < 8; ++i) {
        char path[32];
        std::snprintf(path, sizeof(path), "/mnt/usb%d", i);
        probe_root(path, 1);
    }
    for (int i = 0; i < 2; ++i) {
        char path[32];
        std::snprintf(path, sizeof(path), "/mnt/ext%d", i);
        probe_root(path, 1);
    }
    probe_root("/mnt/disc", 1);
    // The same aliases through the path ShadowMountPlus mounts them at:
    // <sandbox>/mnt/usb0..7, <sandbox>/mnt/ext0..1, <sandbox>/mnt/disc,
    // where <sandbox> is /app0's parent.
    probe_root("/app0/..", 0);
    probe_root("/app0/../mnt", 0);
    // Our own data root (nullfs, writable for PPSA titles) and the LAN inbox:
    // the identity's folder, never a second copy of it.
    probe_root("/data", 0);
    probe_root(kDataDir, 0);
    probe_root(data_path("inbox"), 0);
    line("viewer: probe sources: done");
}

// ---- scene -----------------------------------------------------------------
bool intrinsics_valid(const splat::Intrinsics& intrinsics) {
    return intrinsics.fx > 0.0f && intrinsics.fy > 0.0f && intrinsics.width > 0 &&
           intrinsics.height > 0;
}

splat::CameraIntrinsics eye_intrinsics(const splat::Intrinsics& capture, uint32_t width,
                                       uint32_t height) {
    splat::CameraIntrinsics out;
    if (!intrinsics_valid(capture)) return out;
    const float sx = static_cast<float>(width) / static_cast<float>(capture.width);
    const float sy = static_cast<float>(height) / static_cast<float>(capture.height);
    return {capture.fx * sx, capture.fy * sy, capture.cx * sx, capture.cy * sy};
}

// A scene that just became the one on screen supplies its own camera (W18 9.2:
// a converted scene used to be drawn with the boot scene's intrinsics). A scene
// without meta intrinsics gets the renderer's 60 degree fallback and no
// capture-frustum fade, the same decision the boot path makes.
void apply_scene_intrinsics(splat::SplatRenderer& renderer, const splat::SceneMeta& meta,
                            uint32_t width, uint32_t height) {
    const splat::CameraIntrinsics eye = eye_intrinsics(meta.intrinsics, width, height);
    splat::CameraIntrinsics capture{};
    uint32_t capture_width = 0;
    uint32_t capture_height = 0;
    if (intrinsics_valid(meta.intrinsics)) {
        capture = {meta.intrinsics.fx, meta.intrinsics.fy, meta.intrinsics.cx,
                   meta.intrinsics.cy};
        capture_width = meta.intrinsics.width;
        capture_height = meta.intrinsics.height;
    }
    renderer.set_intrinsics(eye, capture, capture_width, capture_height);
    line("viewer: renderer intrinsics fx %.2f fy %.2f for %ux%u, capture %ux%u", eye.fx, eye.fy,
         width, height, capture_width, capture_height);
}

bool shader_dir_usable(const char* directory) {
    const std::string probe = std::string(directory) + "/project.comp.spv";
    struct stat st {};
    return ::stat(probe.c_str(), &st) == 0;
}

// ---- frame copy ------------------------------------------------------------
// The renderer's target image is a RADV image; VideoOut needs its own tile
// layout. The frame goes image -> host-visible RADV buffer on the GPU (the
// shared CopyPass, also used by the VR eye-buffer handoff), then
// VideoOutDisplay::present writes the tiles and flips.
struct TvReadback {
    splat::ps5::CopyPass pass;
    splat::VulkanBuffer buffer;
};

void destroy_tv_readback(splat::VulkanContext& context, TvReadback& readback) {
    readback.pass.destroy();
    if (context.device() != VK_NULL_HANDLE) {
        splat::destroy_vulkan_buffer(context, readback.buffer);
    }
}

bool create_tv_readback(splat::VulkanContext& context, VkDeviceSize bytes, TvReadback& readback,
                        std::string& error) {
    if (!readback.pass.create(context.device(), context.queue_family(), context.queue(), error)) {
        return false;
    }
    auto buffer = splat::create_vulkan_buffer(context, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!buffer.ok()) {
        error = buffer.message();
        return false;
    }
    readback.buffer = buffer.take();
    return true;
}

// ---- run -------------------------------------------------------------------
struct RunState {
    bool gpu_ok = true;  // cleared when a GPU wait timed out: teardown then leaks
};

// Stops the title through the only route a title has, and never returns.
[[noreturn]] void end_title(int status, const char* reason) {
    line("viewer: %s, exit %d", reason, status);
    line("viewer: done, exit %d", status);
    sceSystemServiceLoadExec("exit", nullptr);
    // LoadExec does not return; a return here would fall back into the title
    // CRT, which a title may not do.
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}

// A quit request written by title.sh stop (or run-viewer.sh) ends the run
// cleanly. Only a file written after this run started counts: the one from a
// previous stop must not end it. Same contract as the bench title.
void start_quit_watch() {
    std::thread([] {
        const std::time_t started = std::time(nullptr);
        for (;;) {
            struct stat st {};
            if (::stat(kQuitPath.c_str(), &st) == 0 && st.st_mtime + 1 >= started) {
                line("viewer: quit requested (%s); ending through LoadExec", kQuitPath.c_str());
                sceSystemServiceLoadExec("exit", nullptr);
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }).detach();
}

// Reads one scene file and logs it. The SCENE= boot and the panel's ViewScene
// both come through here, so a scene is read, checked and reported the same way
// whichever path asked for it.
bool read_scene_file(const splat::Path& scene_path, const std::string& name,
                     splat::LoadedScene& loaded) {
    const double started = now_ms();
    auto result = splat::read_scene_path(scene_path);
    if (!result.ok()) {
        line("viewer: FAIL load %s: %s", scene_path.string().c_str(), result.message().c_str());
        return false;
    }
    loaded = std::move(result.value());
    const double load_ms = now_ms() - started;
    line("viewer: scene %s: %zu splats, load %.1f ms", name.c_str(), loaded.scene.count(),
         load_ms);
    if (intrinsics_valid(loaded.meta.intrinsics)) {
        line("viewer: capture intrinsics fx %.2f fy %.2f cx %.1f cy %.1f, image %ux%u",
             loaded.meta.intrinsics.fx, loaded.meta.intrinsics.fy, loaded.meta.intrinsics.cx,
             loaded.meta.intrinsics.cy, loaded.meta.intrinsics.width,
             loaded.meta.intrinsics.height);
    } else {
        line("viewer: no capture intrinsics in meta.json; using a 60 degree horizontal field of "
             "view");
    }
    return true;
}

// The TV panel's one-deep staging slot (scene_switch.h): the renderer's
// stage/commit pair over the playlist entry the panel asked for. TV does not
// preload ahead, so `staged()` is only ever the index an upload put there.
class TvSwitchSlot : public viewer::SwitchSlot {
public:
    TvSwitchSlot(splat::SplatRenderer& renderer, const splat::ScenePlaylist& playlist)
        : renderer_(renderer), playlist_(playlist) {}

    long staged() const override { return staged_; }
    bool holds_buffers() const override { return renderer_.has_staged_scene(); }
    // The meta.json of the last scene this slot read (show_scene and L1/R1 use
    // it for the scene's own intrinsics once the switch has committed).
    const splat::SceneMeta& meta() const { return meta_; }

    void release() override {
        renderer_.discard_staged_scene();
        staged_ = -1;
    }

    bool upload(std::size_t index) override {
        const splat::PlaylistEntry& entry = playlist_.entry(index);
        splat::LoadedScene loaded;
        if (!read_scene_file(entry.scene_file, entry.folder.filename().string(), loaded)) {
            return false;
        }
        const splat::Status staged = renderer_.stage_scene(loaded.scene);
        if (!staged.ok()) {
            line("viewer: FAIL stage scene %zu: %s", index, staged.message().c_str());
            return false;
        }
        meta_ = loaded.meta;
        staged_ = static_cast<long>(index);
        return true;
    }

    bool activate(std::size_t index) override {
        if (staged_ != static_cast<long>(index)) return false;
        staged_ = -1;
        if (!renderer_.has_staged_scene()) return false;  // bookkeeping was stale
        const splat::Status status = renderer_.commit_staged_scene();
        if (!status.ok()) {
            line("viewer: FAIL commit scene %zu: %s", index, status.message().c_str());
            renderer_.discard_staged_scene();
            return false;
        }
        return true;
    }

private:
    splat::SplatRenderer& renderer_;
    const splat::ScenePlaylist& playlist_;
    splat::SceneMeta meta_;
    long staged_ = -1;
};

// ---- W18 convert spike: S1, in-process --------------------------------------
//
// viewer.env (TV mode with UI=1):
//
//   CONVERT_SPIKE=<photo on the console>     relative paths resolve under
//                                            /data/ps5-splat-vr
//   CONVERT_SPIKE_OUT=<scene folder>         where write_scene_folder puts it
//   SCENE=<folder>                           optional: this scene is loaded into
//                                            the renderer BEFORE the conversion
//                                            (the memory variant: a live
//                                            1.2 M-splat scene + SHARP)
//
// Order, which is the experiment: the renderer, VideoOut and the panel come up
// first and the panel draws "converting <photo>" with a progress bar; 2 s later
// a worker thread opens the SHARP GGUF and runs viewer::ConvertSession next to
// the live renderer, so ggml-vulkan creates its own VkDevice over the same
// direct-memory heap. The loop keeps presenting frames throughout and dumps
// convert-start / convert-mid / convert-done; on success the new scene is loaded
// into the renderer, convert-view is dumped, and it is viewed for 20 s. The
// verdict is one line: `spike: S1 PASS <seconds> <splats>` or
// `spike: S1 FAIL <where>`.
//
// The panel is a plain IPanelContent like UiApp (ui_host.h), so the canvas,
// upload and quad path is the one W8 built; only the content differs.
class SpikePanel : public splat::IPanelContent {
public:
    enum Phase { Waiting = 0, Loading = 1, Converting = 2, Done = 3, Failed = 4 };

    explicit SpikePanel(std::string photo) : photo_(std::move(photo)) {}

    // The panel's own clock: a 4 Hz spinner is the only motion while the bar
    // stands still, and it stops once the conversion has a verdict (a hidden
    // panel then costs no texture upload at all).
    void update(const splat::UiInput& input) override {
        if (phase_.load() >= static_cast<int>(Done)) return;
        clock_ += input.dt;
        const int tick = static_cast<int>(clock_ * 4.0);
        if (tick != tick_) {
            tick_ = tick;
            dirty_.store(true);
        }
    }

    bool draw(splat::UiCanvas& canvas) override {
        const int phase = phase_.load();
        const float progress = progress_.load();
        const int percent = static_cast<int>(progress * 100.0f + 0.5f);
        std::string stage;
        std::string detail;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stage = stage_;
            detail = detail_;
        }
        if (!dirty_.load() && phase == drawn_phase_ && percent == drawn_percent_ &&
            tick_ == drawn_tick_ && stage == drawn_stage_) {
            return false;
        }
        drawn_phase_ = phase;
        drawn_percent_ = percent;
        drawn_tick_ = tick_;
        drawn_stage_ = stage;
        dirty_.store(false);

        const splat::Rgba background{16, 18, 22, 255};
        const splat::Rgba text{232, 234, 238, 255};
        const splat::Rgba dim{140, 146, 158, 255};
        const splat::Rgba track{38, 42, 50, 255};
        const splat::Rgba fill{96, 190, 130, 255};
        const splat::Rgba warn{214, 116, 96, 255};
        canvas.clear(background);
        const int left = 72;
        canvas.text(left, 64, "ps5-splat-vr   SHARP convert spike: S1, in process", 2, dim);
        canvas.text(left, 120, ("converting " + photo_).c_str(), 3, text);

        const char spinner[5] = {'|', '/', '-', '\\', '\0'};
        char status[256];
        switch (phase) {
            case Loading:
                std::snprintf(status, sizeof(status), "%c loading: %s", spinner[tick_ & 3],
                              stage.c_str());
                break;
            case Converting:
                std::snprintf(status, sizeof(status), "%c %s, %d%%", spinner[tick_ & 3],
                              stage.c_str(), percent);
                break;
            case Done:
                std::snprintf(status, sizeof(status), "done: %u splats in %.1f s",
                              splats_.load(), seconds_.load());
                break;
            case Failed:
                std::snprintf(status, sizeof(status), "FAILED: %s", detail.c_str());
                break;
            default:
                std::snprintf(status, sizeof(status), "waiting for the first frame");
                break;
        }
        canvas.text(left, 200, status, 2, phase == Failed ? warn : text);
        canvas.bar(left, 248, 900, 36, phase == Done ? 1.0f : progress, track, fill);
        canvas.text(left + 920, 254, (std::to_string(percent) + "%").c_str(), 2,
                    phase == Done ? fill : dim);

        // What the spike is: the same text in convert-start, -mid and -done, so
        // the three PNGs differ only by the bar and the stage line above.
        canvas.text(left, 340, "renderer: presenting this frame on the TV", 2, dim);
        canvas.text(left, 376, "converter: SHARP on ggml-vulkan, its own VkDevice", 2, dim);
        canvas.text(left, 412, "bar: fed by the converter's progress callback", 2, dim);
        canvas.text(left, 620, "viewer.env CONVERT_SPIKE / CONVERT_SPIKE_OUT", 2, dim);
        return true;
    }

    // Worker -> panel. The bar fraction is one atomic read per frame; the two
    // text strings (stage, detail) are short and only change at stage/verdict
    // boundaries, so they sit behind a mutex.
    void begin(Phase phase, const std::string& stage) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stage_ = stage;
        }
        phase_.store(static_cast<int>(phase));
        dirty_.store(true);
    }
    void set_stage(const std::string& stage) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stage_ = stage;
        }
        dirty_.store(true);
    }
    void set_progress(float fraction) { progress_.store(fraction); }
    void finish(bool ok, const std::string& detail, double seconds, uint32_t splats) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            detail_ = detail;
        }
        seconds_.store(seconds);
        splats_.store(splats);
        if (ok) progress_.store(1.0f);
        phase_.store(static_cast<int>(ok ? Done : Failed));
        dirty_.store(true);
    }

    int phase() const { return phase_.load(); }
    float progress() const { return progress_.load(); }
    double seconds() const { return seconds_.load(); }
    uint32_t splats() const { return splats_.load(); }
    std::string detail() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return detail_;
    }

private:
    std::string photo_;
    std::atomic<int> phase_{static_cast<int>(Waiting)};
    std::atomic<float> progress_{0.0f};
    std::atomic<double> seconds_{0.0};
    std::atomic<uint32_t> splats_{0};
    std::atomic<bool> dirty_{true};
    mutable std::mutex mutex_;
    std::string stage_ = "waiting";
    std::string detail_;
    double clock_ = 0.0;
    int tick_ = 0;
    int drawn_phase_ = -1;
    int drawn_percent_ = -1;
    int drawn_tick_ = -1;
    std::string drawn_stage_;
};

// The model file: the manager's one truth first (state/models/<backend>.gguf,
// docs/ui-v1-plan.md section 2), then the legacy models/ names the bench and W4
// used. The first that exists wins; empty means there is nothing to open.
std::string resolve_sharp_gguf() {
    const std::string candidates[] = {
        std::string(kDataDir) + "/state/models/sharp.gguf",
        std::string(kDataDir) + "/models/sharp.gguf",
        std::string(kDataDir) + "/models/sharp_2572gikvuh-f16-v1.gguf",
        std::string(kDataDir) + "/models/sharp_2572gikvuh-f32-v1.gguf",
    };
    for (const std::string& path : candidates) {
        struct stat st {};
        if (::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) return path;
    }
    return {};
}

// ggml-vulkan's own view of the direct-memory heap. free/total are the numbers
// the bench logs before and after a model load; on this console the heap is the
// 11.44 GiB the winsys reports (docs/research/spike-b-memory.md).
void log_ggml_heap(const char* when) {
    size_t free_bytes = 0;
    size_t total_bytes = 0;
    ggml_backend_vk_get_device_memory(0, &free_bytes, &total_bytes);
    line("spike: ggml heap %s: %s free of %s", when,
         splat::bench::human_bytes(free_bytes).c_str(),
         splat::bench::human_bytes(total_bytes).c_str());
}

// The worker: ggml's device, the model, one photo, unload. It runs beside the
// renderer, which is the whole point of S1; every failure is handed to the panel
// and reported once by the main thread (which owns the verdict lines).
void run_convert_job(SpikePanel& panel, const std::string& photo, const std::string& out_folder) {
    const double started = now_ms();
    auto fail = [&panel](const std::string& where) {
        line("viewer: spike: conversion failed: %s", where.c_str());
        panel.finish(false, where, 0.0, 0);
    };

    const std::string gguf = resolve_sharp_gguf();
    if (gguf.empty()) {
        fail("no SHARP GGUF under " + std::string(kDataDir) + "/{state/models,models}");
        return;
    }
    line("spike: model %s", gguf.c_str());

    // ggml's device comes up here, after the renderer's: two RADV users in one
    // title process. The device description is what the bench logs too.
    panel.begin(SpikePanel::Loading, "opening the ggml Vulkan device");
    const double device_started = now_ms();
    ggml_backend_dev_t device = ggml_backend_dev_by_name("Vulkan0");
    if (device == nullptr) {
        fail("no ggml Vulkan device (RADV init failed; see the [radv] lines)");
        return;
    }
    line("spike: ggml device %s up in %.2f s", ggml_backend_dev_description(device),
         (now_ms() - device_started) / 1000.0);
    log_ggml_heap("after device");

    panel.set_stage("loading the model");
    line("spike: model load starting (peak RSS %s)",
         splat::bench::human_bytes(splat::bench::peak_rss_bytes()).c_str());
    viewer::ConvertSession session;
    const splat::Status opened = session.open(splat::Path(gguf));
    if (!opened.ok()) {
        fail("model load: " + opened.message());
        return;
    }
    line("spike: model loaded in %.2f s (peak RSS %s)", (now_ms() - started) / 1000.0,
         splat::bench::human_bytes(splat::bench::peak_rss_bytes()).c_str());
    log_ggml_heap("after load");
    log_pools();

    panel.begin(SpikePanel::Converting, "starting inference");
    splat::Progress progress([&panel](float fraction, const std::string& stage) {
        panel.set_progress(fraction);
        panel.set_stage(stage.empty() ? std::string("converting") : stage);
    });
    const double run_started = now_ms();
    splat::Result<viewer::ConvertOutcome> outcome =
        session.run(splat::Path(photo), splat::Path(out_folder), "", &progress);
    line("spike: conversion %s in %.2f s, peak RSS %s", outcome.ok() ? "ran" : "failed",
         (now_ms() - run_started) / 1000.0,
         splat::bench::human_bytes(splat::bench::peak_rss_bytes()).c_str());

    // The unload is part of the measurement: S1 requires the ggml device's
    // memory to be back before the renderer allocates the next scene.
    session.close();
    line("spike: model unloaded after %.2f s (peak RSS %s)", (now_ms() - started) / 1000.0,
         splat::bench::human_bytes(splat::bench::peak_rss_bytes()).c_str());
    log_ggml_heap("after unload");
    log_pools();

    if (!outcome.ok()) {
        fail(outcome.message());
        return;
    }
    line("spike: %s", outcome.value().message.c_str());
    panel.finish(true, outcome.value().message, outcome.value().seconds, outcome.value().splats);
}

// The last path component, for the panel's "converting <name>" and for the
// gallery lookup of the folder the conversion wrote.
std::string last_component(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

// app_crt's _start(process_parameters, loader_teardown) calls this as main().
// A title gets no arguments from the launch route, so viewer.env is the input.
int main(int, char**) {
    // The title's own version, compiled in by the viewer Makefile: the one
    // place a klog line (and so a bug report) can name what is running.
#ifndef SPLAT_VERSION
#define SPLAT_VERSION "0.0.0-unknown"
#endif
    line("viewer: start (ps5-splat-vr v%s, C6 renderer, RADV linked directly, no loader)",
         SPLAT_VERSION);
    sceSystemServiceHideSplashScreen();

    // Before anything GPU: the pool RADV's winsys reports as its heap.
    log_pools();

    uint32_t sdk = 0;
    size_t sdk_size = sizeof(sdk);
    if (sysctlbyname("kern.sdk_version", &sdk, &sdk_size, nullptr, 0) == 0 && sdk) {
        line("viewer: firmware %x.%02x (kern.sdk_version 0x%08x)", sdk >> 24, (sdk >> 16) & 0xff,
             sdk);
    } else {
        line("viewer: firmware unknown (kern.sdk_version unavailable)");
    }

    Options options;
    const std::string env_text = load_env_file(kEnvPath);
    options.env_ok = !env_text.empty();
    if (!options.env_ok) {
        line("viewer: no %s (using defaults: mode vr, the first scene of the gallery, no "
             "auto-exit)",
             kEnvPath.c_str());
    }
    const std::string mode = env_value(env_text, "MODE");
    if (!mode.empty()) options.mode = mode;
    for (char& character : options.mode) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    if (options.mode != "vr" && options.mode != "tv") {
        line("viewer: MODE=%s is neither vr nor tv; using vr", options.mode.c_str());
        options.mode = "vr";
    }
    uint32_t number = 0;
    if (parse_u32(env_value(env_text, "SECONDS"), number)) {
        options.seconds = static_cast<int>(number);
        options.seconds_given = true;
    }
    if (parse_u32(env_value(env_text, "WIDTH"), number)) options.width = number;
    if (parse_u32(env_value(env_text, "HEIGHT"), number)) options.height = number;
    if (parse_u32(env_value(env_text, "FLIP_RATE"), number)) options.flip_rate = static_cast<int>(number);
    float real = 0.0f;
    if (parse_float(env_value(env_text, "COMFORT_RADIUS"), real)) {
        options.comfort_radius = real;
        options.comfort_given = true;
    }
    if (parse_float(env_value(env_text, "WORLD_SCALE"), real)) options.world_scale = real;
    if (parse_float(env_value(env_text, "SWAY_PERIOD"), real)) options.sway_period = real;
    if (parse_u32(env_value(env_text, "PRELOAD"), number)) options.preload = number != 0;
    if (parse_u32(env_value(env_text, "MIRROR"), number)) options.mirror = number != 0;
    if (parse_u32(env_value(env_text, "UI"), number)) {
        options.ui = number != 0;
        options.ui_given = true;
    }
    if (parse_u32(env_value(env_text, "UI_FAKE"), number)) options.ui_fake = number != 0;
    options.script = env_value(env_text, "SCRIPT");
    const std::string shots_dir = resolve_data_path(env_value(env_text, "SHOTS_DIR"));
    if (!shots_dir.empty()) options.shots_dir = shots_dir;
    // W18 convert spike (TV + UI=1): the photo on the console and the scene
    // folder the in-process conversion writes (see "W18 convert spike" above).
    // Locals, not viewer::Options: no other path reads them, and Options stays
    // the keys both modes share (ui_setup.h).
    const std::string convert_spike = resolve_data_path(env_value(env_text, "CONVERT_SPIKE"));
    const std::string convert_spike_out =
        resolve_data_path(env_value(env_text, "CONVERT_SPIKE_OUT"));
    options.scene = env_value(env_text, "SCENE");
    const std::string scenes_dir = env_value(env_text, "SCENES_DIR");
    if (!scenes_dir.empty()) options.scenes_dir = scenes_dir;
    // MODE=vr defaults, after every key has been read: SECONDS absent means no
    // auto-exit (the v0.1 release leaked the dev bench's 30-minute cap into it,
    // so a fresh install quit to the home screen half an hour in), and
    // COMFORT_RADIUS absent is the spec's standing VR radius (the TV path keeps
    // 0.25 m). viewer_env_defaults.h holds both rules, which the desktop tests
    // drive as well.
    if (options.mode == "vr") {
        const viewer::VrDefaults vr_defaults = viewer::apply_vr_defaults(
            options.seconds_given, options.seconds, options.comfort_given,
            options.comfort_radius);
        options.seconds = vr_defaults.seconds;
        options.comfort_radius = vr_defaults.comfort_radius;
    }
    if (options.mode == "vr") {
        line("viewer: config %s: mode=vr scene=%s seconds=%d comfort_radius=%.3f m world_scale=%.2f "
             "preload=%d mirror=%d",
             options.env_ok ? kEnvPath.c_str() : "(defaults)",
             options.scene.empty() ? "(first)" : options.scene.c_str(), options.seconds,
             options.comfort_radius, options.world_scale, options.preload ? 1 : 0,
             options.mirror ? 1 : 0);
    } else {
        line("viewer: config %s: mode=tv scene=%s seconds=%d out=%ux%u flip_rate=%d "
             "comfort_radius=%.3f m sway_period=%.1f s",
             options.env_ok ? kEnvPath.c_str() : "(defaults)",
             options.scene.empty() ? "(first)" : options.scene.c_str(), options.seconds,
             options.width, options.height, options.flip_rate, options.comfort_radius,
             options.sway_period);
    }

    // T7 spike: an explicit request in viewer.env, before any GPU call, so the
    // answer lands in the log even when the rest of the run fails.
    if (env_value(env_text, "PROBE") == "sources") probe_sources();

    // W31: the sample scenes a release package carries (/app0/samples) go into
    // the gallery as the "Samples" album on the first start, once. It runs
    // before any GPU work and for both modes, so a fresh install has something
    // to browse even when it boots straight into a scene; the marker under the
    // data dir makes a later deletion final. A package without samples has no
    // such folder and this is a quiet no-op. UI_FAKE=1 is the deterministic
    // panel test and writes nothing on the console.
    if (!options.ui_fake) {
        const splat::SamplesSeed seed =
            splat::seed_samples(splat::Path(viewer::kSamplesDir),
                                splat::Path(options.scenes_dir),
                                splat::Path(viewer::data_path("state/samples.seeded")));
        if (!seed.ok()) {
            line("viewer: samples: %s", seed.error.c_str());
        } else if (seed.scenes > 0) {
            line("viewer: samples: copied %zu scene(s) into %s", seed.scenes,
                 splat::kSamplesAlbumName);
        }
    }

    // RADV's two title settings, before the first GPU call (PS5LM's order).
    // overwrite = 0, so a viewer.env entry can override it (bench_env.cpp does
    // the same); without it RADV splits the APU pool into a 2/3 + 1/3 pair.
    setenv("radv_enable_unified_heap_on_apu", "true", 0);
    const char* unified = getenv("radv_enable_unified_heap_on_apu");
    line("viewer: radv_enable_unified_heap_on_apu=%s (one heap over the direct-memory pool)",
         unified != nullptr ? unified : "true");
    line("viewer: driver stderr capture %s",
         ps5_klog_capture_stderr("[radv]") == 0 ? "on" : "off");

    // The six SPIR-V modules both modes load, from the package or from /data.
    const std::string shader_dir = shader_dir_usable(kAppShaderDir) ? kAppShaderDir : kDataShaderDir;
    if (!shader_dir_usable(shader_dir.c_str())) {
        line("viewer: FAIL shaders: no project.comp.spv in %s or %s (the package ships them under "
             "shaders/)",
             kAppShaderDir, kDataShaderDir.c_str());
        end_title(6, "FAIL no shaders");
    }

    // ---- VR mode ----------------------------------------------------------
    // The default: the same scene on the PSVR2, rendered stereo by the same C6
    // renderer and handed to Hmd2 as its eye buffers (vr_mode.cpp). It owns its
    // device, display and teardown, so nothing below runs for it.
    if (options.mode == "vr") {
        viewer::VrOptions vr;
        vr.scene = options.scene;
        vr.scenes_dir = options.scenes_dir;
        vr.shader_dir = shader_dir;
        vr.comfort_radius = options.comfort_radius;
        vr.world_scale = options.world_scale;
        vr.preload = options.preload;
        vr.mirror = options.mirror;
        // SECONDS is the session's only cap: 0 (the default, and an explicit
        // SECONDS=0) means no auto-exit at all; the panel's Quit and the quit
        // file still end the session. A dev run caps itself with SECONDS=1800.
        vr.cap_seconds = viewer::vr_cap_seconds(options.seconds);
        line("viewer: mode vr (MODE=vr in %s); TV mode is MODE=tv",
             options.env_ok ? kEnvPath.c_str() : "(defaults)");
        const int status = viewer::run_vr(vr);
        end_title(status, status == 0 ? "vr done" : "vr FAIL");
    }

    // ---- TV mode ----------------------------------------------------------
    // MODE=tv: today's behaviour, a mono 1920x1080 VideoOut frame swaying inside
    // the comfort radius. The quit file ends this path directly (title.sh stop),
    // so it is watched here rather than in VR mode, which tears the headset down.
    start_quit_watch();

    // ---- UI + scripted input (UI=1) ---------------------------------------
    // The panel is UiApp (core/ui: menu, albums, scenes) over the same action
    // map a pad uses; a SCRIPT line replays button words through it, and `shot`
    // lines dump the finished TV frame. Without UI=1 nothing here runs: the TV
    // path is unchanged.
    const bool ui_enabled = options.ui;
    // W18: the convert spike is a UI=1 TV run whose panel content is SpikePanel
    // instead of UiApp. No script, no services, no pad: the worker drives it.
    const bool spike_enabled = ui_enabled && !convert_spike.empty();
    if (spike_enabled && convert_spike_out.empty()) {
        line("viewer: FAIL spike: CONVERT_SPIKE=%s without CONVERT_SPIKE_OUT",
             convert_spike.c_str());
        end_title(9, "FAIL spike out");
    }
    if (ui_enabled && spike_enabled && !options.script.empty()) {
        line("viewer: spike: SCRIPT=%s is ignored (the spike drives the panel itself)",
             options.script.c_str());
    }
    std::unique_ptr<splat::ScriptPlayer> script_player;
    if (ui_enabled && !spike_enabled) {
        if (!ensure_directory(options.shots_dir)) {
            line("viewer: FAIL shots dir %s: errno %d (%s)", options.shots_dir.c_str(), errno,
                 std::strerror(errno));
            end_title(9, "FAIL shots dir");
        }
        if (!options.script.empty()) {
            const std::string script_text = read_text_file(options.script);
            if (script_text.empty()) {
                line("viewer: FAIL script %s: the file is missing or empty",
                     options.script.c_str());
                end_title(9, "FAIL script");
            }
            auto parsed = splat::parse_input_script(script_text);
            if (!parsed.ok()) {
                line("viewer: FAIL script %s: %s", options.script.c_str(),
                     parsed.message().c_str());
                end_title(9, "FAIL script");
            }
            line("viewer: script %s: %zu event(s), %.1f s; shots %s", options.script.c_str(),
                 parsed.value().events().size(), parsed.value().duration(),
                 options.shots_dir.c_str());
            script_player = std::make_unique<splat::ScriptPlayer>(parsed.take());
        } else {
            line("viewer: ui=1 without SCRIPT; shots would go to %s",
                 options.shots_dir.c_str());
        }
    }
    if (spike_enabled) {
        if (!ensure_directory(options.shots_dir)) {
            line("viewer: FAIL shots dir %s: errno %d (%s)", options.shots_dir.c_str(), errno,
                 std::strerror(errno));
            end_title(9, "FAIL shots dir");
        }
        // ggml's own lines (device description, coopmat/subgroup facts) are how
        // the bench logs its device; the same bridge is used here.
        splat::bench::install_ggml_logging("viewer");
        line("viewer: spike: S1 in process, photo %s, out %s, shots %s",
             convert_spike.c_str(), convert_spike_out.c_str(),
             options.shots_dir.c_str());
    }

    // ---- scene ------------------------------------------------------------
    // UI=1 boots into the panel with no scene loaded: the TV panel fills the
    // frame over a dark backdrop, and ViewScene (the loop below) loads one into
    // the renderer. Without UI the SCENE= path is unchanged: the playlist is
    // built and one scene is read before the device is created.
    const double scene_started = now_ms();
    std::optional<splat::LoadedScene> boot_scene;
    // UI=1 loads no scene (the panel is over the dark backdrop). The one
    // exception is the spike's memory variant: CONVERT_SPIKE plus SCENE= loads
    // that scene into the renderer BEFORE the converter starts, so the run
    // answers "does a live 1.2 M-splat scene and SHARP fit together?".
    const bool spike_boot_scene = spike_enabled && !options.scene.empty();
    if (!ui_enabled || spike_boot_scene) {
        auto playlist = splat::ScenePlaylist::from_folder(splat::Path(options.scenes_dir));
        if (!playlist.ok()) {
            line("viewer: FAIL scenes: %s", playlist.message().c_str());
            end_title(2, "FAIL no scenes");
        }
        splat::ScenePlaylist& scenes = playlist.value();
        line("viewer: scenes: %zu folder(s) under %s", scenes.size(), options.scenes_dir.c_str());
        size_t scene_index = 0;
        if (!options.scene.empty()) {
            bool found = false;
            for (size_t i = 0; i < scenes.size(); ++i) {
                if (scenes.entry(i).folder.filename().string() == options.scene) {
                    scene_index = i;
                    found = true;
                    break;
                }
            }
            if (!found) {
                line("viewer: SCENE=%s is not one of the %zu folders under %s; using the first",
                     options.scene.c_str(), scenes.size(), options.scenes_dir.c_str());
            }
        }
        scenes.set_index(scene_index);
        const splat::PlaylistEntry& entry = scenes.current();
        line("viewer: scene %zu/%zu %s", scene_index + 1, scenes.size(),
             entry.folder.filename().string().c_str());
        boot_scene.emplace();
        if (!read_scene_file(entry.scene_file, entry.folder.filename().string(), *boot_scene)) {
            end_title(3, "FAIL scene load");
        }
    }

    // ---- device -----------------------------------------------------------
    const double device_started = now_ms();
    splat::VulkanContextOptions context_options;
    auto context_result = splat::VulkanContext::create(context_options);
    if (!context_result.ok()) {
        line("viewer: FAIL device: %s", context_result.message().c_str());
        end_title(4, "FAIL no device");
    }
    std::unique_ptr<splat::VulkanContext> context = context_result.take();
    const VkPhysicalDeviceProperties& device_properties = context->properties();
    line("viewer: device %s, Vulkan %u.%u.%u, driver 0x%08x", device_properties.deviceName,
         VK_API_VERSION_MAJOR(device_properties.apiVersion),
         VK_API_VERSION_MINOR(device_properties.apiVersion),
         VK_API_VERSION_PATCH(device_properties.apiVersion), device_properties.driverVersion);
    const VkPhysicalDeviceMemoryProperties& memory = context->memory_properties();
    line("viewer: memory: %u heaps, %u types", memory.memoryHeapCount, memory.memoryTypeCount);
    for (uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
        line("viewer: heap[%u] %.2f GiB (%llu bytes)%s", i,
             gib(static_cast<int64_t>(memory.memoryHeaps[i].size)),
             static_cast<unsigned long long>(memory.memoryHeaps[i].size),
             (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " device-local" : "");
    }
    line("viewer: device up in %.1f ms", now_ms() - device_started);

    // ---- the TV -----------------------------------------------------------
    splat::ps5::VideoOutConfig video_config;
    video_config.width = options.width;
    video_config.height = options.height;
    video_config.flip_rate = options.flip_rate;
    splat::ps5::VideoOutDisplay display;
    std::string error;
    if (!display.open(video_config, error)) {
        line("viewer: FAIL videoout: %s", error.c_str());
        end_title(5, "FAIL videoout");
    }
    line("viewer: videoout: %ux%u, %d buffer(s) of %.2f MiB, flip rate %d", display.width(),
         display.height(), video_config.buffers,
         static_cast<double>(display.buffer_bytes()) / (1024.0 * 1024.0), video_config.flip_rate);
    if (!error.empty()) {
        line("viewer: videoout: %s", error.c_str());
        error.clear();
    }

    // ---- teardown ---------------------------------------------------------
    // Everything the teardown has to free, declared here so the one teardown
    // below can also run from the setup failures that come after VideoOut is
    // open. Whatever does not exist yet is null, and the destroy helpers skip
    // nulls.
    splat::VulkanImage target_image;
    TvReadback readback;
    std::unique_ptr<splat::SplatRenderer> renderer;
    RunState run;
    // W19: a conversion still running when the loop ends (quit, SECONDS, a
    // failed frame) owns its own VkDevice over the same heap. Set below, read by
    // the teardown: the renderer is then leaked rather than freed under a GPU
    // that may still be writing, exactly as W18's spike did for its worker.
    bool jobs_running = false;
    auto teardown = [&]() {
        const double started = now_ms();
        // The display goes first and on every path: blank, unregister, close,
        // unmap, release. Its state is the kernel's, not the GPU's, so it is
        // given back even when a GPU wait timed out; a title that reaches
        // LoadExec with VideoOut still registered is what the review called
        // out (the next title's sceVideoOutOpen then fails).
        std::string video_error;
        display.close(&video_error);
        if (!video_error.empty()) line("viewer: videoout close: %s", video_error.c_str());
        if (run.gpu_ok && !jobs_running) {
            if (context) {
                splat::destroy_vulkan_image(*context, target_image);
                destroy_tv_readback(*context, readback);
            }
            renderer.reset();
            context.reset();
        } else if (jobs_running) {
            // W19: the converter's worker is still inside ggml-vulkan, with its
            // own device and ~7.5 GiB of the heap. The display is already closed
            // (kernel state, above); the renderer and the device are left to the
            // process exit instead of being freed under it.
            line("viewer: teardown: leaking the renderer, its buffers and the device (a conversion "
                 "was still running; nothing may be freed under it)");
        } else {
            // A GPU wait timed out: the device may still be running, so nothing
            // this title allocated is freed or destroyed anywhere. The process
            // ends and the system reclaims it; the log says what was leaked.
            line("viewer: teardown: leaking the renderer, its buffers and the device (a GPU wait "
                 "timed out; nothing may be freed under it)");
        }
        line("viewer: teardown %.1f ms", now_ms() - started);
    };

    // ---- renderer ---------------------------------------------------------
    splat::RendererOptions renderer_options;
    renderer_options.shader_dir = shader_dir;
    renderer_options.eye_width = display.width();
    renderer_options.eye_height = display.height();
    renderer_options.stereo = false;
    // B8G8R8A8: the byte order VideoOut scans out, and an sRGB target so the
    // renderer's premultiplied blend runs in linear light (docs/render.md).
    renderer_options.target_format = VK_FORMAT_B8G8R8A8_SRGB;
    // Without UI the renderer is built for the boot scene's capture intrinsics,
    // exactly as before. UI=1 has no scene at boot (the panel is over the dark
    // backdrop), so it keeps the renderer's 60 degree fallback and ViewScene
    // loads a scene into the renderer's one-deep staging slot later.
    splat::SplatScene empty_scene;
    const splat::SceneMeta* boot_meta = boot_scene ? &boot_scene->meta : nullptr;
    if (boot_meta != nullptr) {
        renderer_options.intrinsics =
            eye_intrinsics(boot_meta->intrinsics, display.width(), display.height());
        if (intrinsics_valid(boot_meta->intrinsics)) {
            renderer_options.capture_intrinsics = {boot_meta->intrinsics.fx,
                                                   boot_meta->intrinsics.fy,
                                                   boot_meta->intrinsics.cx,
                                                   boot_meta->intrinsics.cy};
            renderer_options.capture_width = boot_meta->intrinsics.width;
            renderer_options.capture_height = boot_meta->intrinsics.height;
        }
    }
    const splat::SplatScene& initial_scene = boot_scene ? boot_scene->scene : empty_scene;
    const double renderer_started = now_ms();
    splat::Result<std::unique_ptr<splat::SplatRenderer>> renderer_result;
    auto create_renderer = [&]() {
        renderer_result = splat::SplatRenderer::create(*context, renderer_options, initial_scene);
    };
    run_with_big_stack(create_renderer, "renderer");
    if (!renderer_result.ok()) {
        line("viewer: FAIL renderer: %s", renderer_result.message().c_str());
        teardown();
        end_title(6, "FAIL no renderer");
    }
    renderer = renderer_result.take();
    line("viewer: renderer: %ux%u mono, shaders %s, create %.1f ms", renderer->eye_width(),
         renderer->eye_height(), shader_dir.c_str(), now_ms() - renderer_started);
    if (!context->timestamps_supported()) {
        line("viewer: GPU timestamps unavailable on this device; project/sort/draw print 0.00 "
             "(fps, copy, tiles and flip are still measured)");
    }

    const VkExtent2D extent = renderer->target_extent();
    auto image_result = splat::create_vulkan_color_image(
        *context, renderer_options.target_format, extent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!image_result.ok()) {
        line("viewer: FAIL target image: %s", image_result.message().c_str());
        teardown();
        end_title(6, "FAIL no target image");
    }
    target_image = image_result.take();
    if (!create_tv_readback(*context, static_cast<VkDeviceSize>(extent.width) * extent.height * 4u,
                            readback, error)) {
        line("viewer: FAIL frame copy: %s", error.c_str());
        teardown();
        end_title(6, "FAIL no readback");
    }

    splat::RenderTarget target;
    target.image = target_image.image;
    target.view = target_image.view;
    target.extent = extent;
    target.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    target.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    // ---- the panel (UI=1) --------------------------------------------------
    // One UiCanvas; the renderer's overlay textures are re-uploaded only when
    // IPanelContent::draw() reports a change, and UiHost::quads() is what this
    // loop merges into set_overlay_quads (the same path vr_mode uses for its
    // label and HUD).
    //
    // UI=1 is the real panel (UiApp, W7/W10 screens) with the gallery paths
    // from viewer.env: SCENES_DIR, the inbox/state under the data root and /mnt
    std::unique_ptr<viewer::UiRuntime> ui_runtime;
    viewer::UiRuntime* ui = nullptr;
    splat::UiApp* ui_app = nullptr;
    viewer::UiHost* ui_host = nullptr;
    // W18: the convert spike has no services and no UiApp to assemble, so it
    // builds no UiRuntime: its SpikePanel is the content and the same UiHost is
    // constructed over it (one canvas, one overlay texture, the path W8 built).
    // ui_setup.h's assembly stays the one the panel itself runs through.
    std::unique_ptr<SpikePanel> spike;
    std::unique_ptr<viewer::UiHost> spike_host;
    if (ui_enabled) {
        if (spike_enabled) {
            spike = std::make_unique<SpikePanel>(last_component(convert_spike));
            spike_host =
                std::make_unique<viewer::UiHost>(*renderer, *spike, viewer::UiHost::Mode::Tv);
            if (!spike_host->init(1280, 720, renderer_options.intrinsics)) {
                line("viewer: FAIL ui host: %s", spike_host->last_error().c_str());
                teardown();
                end_title(9, "FAIL ui host");
            }
            ui_host = spike_host.get();
            line("viewer: ui: spike panel, photo %s, scene %s", convert_spike.c_str(),
                 convert_spike_out.c_str());
        } else {
            ui_runtime = std::make_unique<viewer::UiRuntime>(viewer::make_ui_runtime(options));
            ui = ui_runtime.get();
            if (!ui->start(*renderer, viewer::UiHost::Mode::Tv, renderer_options.intrinsics)) {
                line("viewer: FAIL ui host: %s", ui->last_error().c_str());
                teardown();
                end_title(9, "FAIL ui host");
            }
            ui_app = ui->app.get();
            ui_host = ui->host.get();
            line("viewer: ui: UiApp at the menu, scenes %s, services %s", options.scenes_dir.c_str(),
                 ui->fake ? "fake (UI_FAKE=1)"
                          : "models, jobs and captures real (ui_setup.cpp)");
        }
        line("viewer: shots: %ux%u target B8G8R8A8_SRGB read back as BGRA, written as RGBA PNG",
             extent.width, extent.height);
    }

    // ---- the conversion gate (W19) -----------------------------------------
    // S1 (W18 4.1-4.3): the renderer, VideoOut, the panel and the viewed scene
    // all stay alive while the runner's worker converts, so release_scene and
    // restore_scene have nothing to do here. The user may keep browsing: only
    // the model-load phase holds a scene switch (viewer::scene_loading_allowed,
    // read by ViewScene and L1/R1 below), because that is the window W18 measured
    // tight (8.65 GiB free). During inference a scene upload fits, and the
    // renderer's stage/commit frees the old scene's buffers when it swaps.
    splat::IJobService* const ui_jobs = ui != nullptr ? ui->services.jobs : nullptr;
    viewer::ConvertGate convert_gate(viewer::ConvertGate::Hooks{
        [] { line("viewer: jobs: a conversion started"); },
        {},  // S1 keeps the viewed scene loaded: W18 run B, 8.65 GiB still free
        [&] {
            // The Queue/Albums screens re-read on enter; this repaints a panel
            // that is on screen (a job ended behind the scene).
            if (ui_app != nullptr) ui_app->invalidate();
        },
        [] { line("viewer: jobs: the queue is idle again"); }});

    // ---- the loop ---------------------------------------------------------
    // Seated at the capture point, swaying sideways inside 60 % of the comfort
    // radius: at the capture pose a shift shows parallax, not rotation, and
    // 60 % is where the comfort fade is still 0.896 (docs/render.md), so the
    // frame stays comfortably visible while moving.
    splat::ViewerState state;
    state.set_comfort_radius(options.comfort_radius);
    const float sway_amplitude = 0.6f * state.comfort_radius();
    const float ipd = 0.063f;

    const double loop_started = now_ms();
    double second_started = loop_started;
    double pass_ms = 0.0;
    double tile_total_ms = 0.0;
    double present_wall_total_ms = 0.0;
    double copy_total_ms = 0.0;
    double tile_second_ms = 0.0;
    double present_second_ms = 0.0;
    double copy_second_ms = 0.0;
    double project_second_ms = 0.0;
    double sort_second_ms = 0.0;
    double draw_second_ms = 0.0;
    uint64_t frames = 0;
    uint64_t frames_this_second = 0;
    float last_fade = 1.0f;
    float last_head_x = 0.0f;
    int status = 0;
    const char* reason = "PASS";
    // UI=1 state: the previous frame's action word (for pressed edges), the
    // script clock (seconds from the first UI frame) and one reuseable RGBA
    // buffer for frame dumps.
    uint32_t ui_previous_actions = 0;
    double ui_last_elapsed = 0.0;
    std::vector<uint8_t> shot_rgba;
    // UI=1 scene: the album playlist the panel last asked for and the renderer
    // staging slot L1/R1 step through (scene_switch.h keeps the switch rule).
    std::unique_ptr<splat::ScenePlaylist> ui_playlist;
    std::unique_ptr<TvSwitchSlot> ui_slot;
    bool menu_quit = false;
    // W18 spike state (main thread): the worker, its verdict and the shot and
    // clock bookkeeping. The worker's own state is the SpikePanel's atomics.
    std::thread spike_thread;
    bool spike_started = false;
    bool spike_joined = false;
    bool spike_ok = false;
    bool spike_fail = false;   // a `spike: S1 FAIL` line was already written
    bool spike_pass = false;
    bool spike_mid = false;
    bool spike_view_loaded = false;
    bool spike_view_shot = false;
    double spike_start_s = 0.0;
    double spike_fps_s = 0.0;
    double spike_view_s = 0.0;
    double spike_view_until = 0.0;
    uint64_t spike_fps_frames = 0;
    std::vector<std::string> spike_shots;
    const std::string spike_out_name =
        spike != nullptr ? last_component(convert_spike_out) : std::string();

    // The renderer's one load path (the same stage/commit pair the panel's
    // ViewScene uses): the album's playlist, stage the entry into the one-deep
    // slot, commit it (a scene switch costs a descriptor swap, not a re-upload),
    // take the screen. nullptr on success, otherwise the panel's toast; the
    // spike calls this with the folder it just converted. SCENES_DIR is the same
    // path make_ui_runtime puts in services.paths.scenes.
    auto show_scene = [&](const std::string& album, int index) -> const char* {
        auto entries = splat::playlist_entries(
            splat::list_scenes(splat::Path(options.scenes_dir), album));
        auto built = splat::ScenePlaylist::from_entries(std::move(entries));
        if (!built.ok()) {
            line("viewer: view scene: album %s has no scenes",
                 album.empty() ? "(loose)" : album.c_str());
            return "No scenes in this album";
        }
        auto next = std::make_unique<splat::ScenePlaylist>(built.take());
        const int last = static_cast<int>(next->size()) - 1;
        next->set_index(static_cast<std::size_t>(std::clamp(index, 0, last)));
        TvSwitchSlot probe(*renderer, *next);
        const viewer::SwitchReport report = viewer::switch_scene(probe, next->index());
        if (!report.switched) {
            line("viewer: FAIL view scene %d of album %s", index,
                 album.empty() ? "(loose)" : album.c_str());
            return "Cannot load the scene";
        }
        // The scene is on screen now: render it with its own capture camera, not
        // the renderer's boot intrinsics (W18 9.2, the converted scene included).
        apply_scene_intrinsics(*renderer, probe.meta(), display.width(), display.height());
        const std::size_t position = next->index() + 1;
        const std::size_t count = next->size();
        const std::string name = next->current().folder.filename().string();
        ui_playlist = std::move(next);
        ui_slot = std::make_unique<TvSwitchSlot>(*renderer, *ui_playlist);
        ui_host->set_visible(false);
        if (ui_app != nullptr) ui_app->set_scene_loaded(true);
        line("viewer: tv scene %zu/%zu %s (album %s)", position, count, name.c_str(),
             album.empty() ? "(loose)" : album.c_str());
        return nullptr;
    };

    for (;;) {
        const double elapsed_s = (now_ms() - loop_started) / 1000.0;
        if (options.seconds > 0 && elapsed_s >= static_cast<double>(options.seconds)) {
            line("viewer: run time %.0f s reached (%d s)", elapsed_s, options.seconds);
            break;
        }
        const float head_x =
            sway_amplitude * std::sin(2.0f * 3.14159265358979f * static_cast<float>(elapsed_s) /
                                      options.sway_period);
        const float head[3] = {head_x, 0.0f, 0.0f};
        const splat::FrameView view = state.frame(head, 0.0f, 0.0f, ipd);
        last_fade = view.comfort_fade;
        last_head_x = head_x;

        // ---- the panel and scripted input (UI=1) --------------------------
        // The script's held button word and stick go through the same action
        // map as a real pad; pressed is the difference against the previous
        // frame. A shot is taken after the frame is read back (below), so the
        // PNG is the frame this input produced.
        bool script_quit = false;
        std::vector<std::string> shots;
        if (ui_enabled) {
            // W19: follow the job service before the panel sees this frame, so a
            // ViewScene from the panel is judged against the state the service is
            // in now.
            if (ui_jobs != nullptr && convert_gate.poll(*ui_jobs)) {
                line("viewer: jobs: %s", viewer::convert_gate_phase_name(convert_gate.phase()));
            }
            splat::UiInput input;
            input.dt = elapsed_s - ui_last_elapsed;
            ui_last_elapsed = elapsed_s;
            if (script_player != nullptr) {
                const splat::ScriptFrame step = script_player->advance(elapsed_s);
                const uint32_t held = step.buttons & ~splat::pad::kIntercepted;
                const uint32_t actions =
                    viewer::pad::ui_actions_for(held, step.lx, step.ly);
                input.pressed = actions & ~ui_previous_actions;
                input.held = actions;
                ui_previous_actions = actions;
                shots = step.shots;
                script_quit = step.quit;
                for (const std::string& mark : step.marks) {
                    line("viewer: script mark %s", mark.c_str());
                }
            }

            // A loaded scene owns the screen: L1/R1 step the album and Options
            // opens the panel again. The panel itself sees no input while it
            // is hidden, so a scene press cannot move a row behind it.
            if (ui_app != nullptr && !ui_host->visible()) {
                if ((input.pressed & (splat::PagePrev | splat::PageNext)) != 0) {
                    if (ui_jobs != nullptr && !viewer::scene_loading_allowed(*ui_jobs)) {
                        // W19: one step of the album is a scene load too, and
                        // only the model-load phase waits for it.
                        ui_app->toast("Converting... scene switch in a moment");
                    } else if (ui_playlist && ui_slot) {
                        const std::size_t before = ui_playlist->index();
                        if ((input.pressed & splat::PagePrev) != 0) ui_playlist->previous();
                        if ((input.pressed & splat::PageNext) != 0) ui_playlist->next();
                        if (ui_playlist->index() != before) {
                            const std::size_t target = ui_playlist->index();
                            const viewer::SwitchReport report =
                                viewer::switch_scene(*ui_slot, target);
                            if (report.switched) {
                                // Scene switching never recentres: the sway
                                // and the viewer pose are untouched.
                                apply_scene_intrinsics(*renderer, ui_slot->meta(),
                                                       display.width(), display.height());
                                line("viewer: tv scene %zu/%zu %s", ui_playlist->index() + 1,
                                     ui_playlist->size(),
                                     ui_playlist->current().folder.filename().string().c_str());
                            } else {
                                ui_playlist->set_index(before);
                                ui_app->toast("Cannot load the next scene");
                                line("viewer: FAIL scene switch to index %zu (stayed on %zu)",
                                     target + 1, before + 1);
                            }
                        }
                    }
                }
                // Options over the scene opens the panel (inside the panel it
                // opens Settings; Circle at the root emits ResumeScene). The
                // input is cleared below, so the app does not also see it.
                if ((input.pressed & splat::Menu) != 0) ui_host->set_visible(true);
                input.pressed = 0;
                input.held = 0;
                input.click = false;
            }

            // ---- the convert spike's own clock (W18) -----------------------
            // (a) panel first: this block runs before UiHost::frame(), so the
            // panel draws the state this frame produced.
            if (spike != nullptr) {
                const int phase = spike->phase();
                // (b) 2 s after the panel came up: the renderer and the TV are
                // live, and ggml-vulkan starts next to them.
                if (!spike_started && elapsed_s >= 2.0) {
                    spike_started = true;
                    spike_start_s = elapsed_s;
                    spike_fps_s = elapsed_s;
                    spike_fps_frames = frames;
                    line("viewer: spike: start at t=%.1f s (renderer %ux%u, %s scene, %u "
                         "visible splats)",
                         elapsed_s, extent.width, extent.height,
                         spike_boot_scene ? "a loaded" : "no", renderer->last_visible_count());
                    log_pools();
                    spike->begin(SpikePanel::Loading, "starting");
                    SpikePanel* panel = spike.get();
                    const std::string photo = convert_spike;
                    const std::string out_folder = convert_spike_out;
                    auto job = [panel, photo, out_folder]() {
                        run_convert_job(*panel, photo, out_folder);
                    };
                    spike_thread = std::thread([job]() mutable {
                        run_with_big_stack(job, "convert");
                    });
                    spike_shots.push_back("convert-start");
                }
                // (c) a mid-conversion frame: a quarter of the way into the
                // inference, while the model is loaded and the GPU is busy.
                if (spike_started && !spike_mid && spike->progress() >= 0.25f) {
                    spike_mid = true;
                    spike_shots.push_back("convert-mid");
                }
                // Frames keep being presented: the spike's own 5 s line, next
                // to the loop's per-second fps line.
                if (spike_started && !spike_joined && elapsed_s - spike_fps_s >= 5.0) {
                    const double span = elapsed_s - spike_fps_s;
                    line("spike: t=%.1f fps=%.1f phase=%d progress=%d%% visible=%u", 
                         elapsed_s - spike_start_s,
                         static_cast<double>(frames - spike_fps_frames) / span, phase,
                         static_cast<int>(spike->progress() * 100.0f), 
                         renderer->last_visible_count());
                    spike_fps_s = elapsed_s;
                    spike_fps_frames = frames;
                }
                // (c/d) the worker's verdict: dump the panel's last frame, then
                // put its result in the renderer.
                if (spike_started && !spike_joined &&
                    (phase == SpikePanel::Done || phase == SpikePanel::Failed)) {
                    spike_ok = phase == SpikePanel::Done;
                    if (spike_thread.joinable()) spike_thread.join();
                    spike_joined = true;
                    spike_shots.push_back("convert-done");
                    line("viewer: spike: worker %s after %.1f s, peak RSS %s",
                         spike_ok ? "done" : "failed", elapsed_s - spike_start_s,
                         splat::bench::human_bytes(splat::bench::peak_rss_bytes()).c_str());
                    if (!spike_ok) {
                        line("spike: S1 FAIL %s", spike->detail().c_str());
                        spike_fail = true;
                    }
                }
                // convert-done is already on disk when the scene goes in, so
                // the two shots differ by what is on screen: the panel's
                // verdict, then the converter's scene.
                if (spike_joined && spike_ok && !spike_view_loaded && spike_shots.empty()) {
                    auto entries = splat::list_scenes(splat::Path(options.scenes_dir), "");
                    int index = -1;
                    for (std::size_t i = 0; i < entries.size(); ++i) {
                        if (entries[i].name == spike_out_name) {
                            index = static_cast<int>(i);
                            break;
                        }
                    }
                    if (index < 0) {
                        line("spike: S1 FAIL scene %s is not listed under %s", spike_out_name.c_str(),
                             options.scenes_dir.c_str());
                        spike_fail = true;
                    } else if (show_scene("", index) != nullptr) {
                        line("spike: S1 FAIL cannot load %s into the renderer",
                             spike_out_name.c_str());
                        spike_fail = true;
                    } else {
                        spike_view_loaded = true;
                        spike_view_s = elapsed_s;
                    }
                }
                // (d) one presented frame of the new scene, then the verdict.
                if (spike_view_loaded && !spike_view_shot && elapsed_s - spike_view_s >= 0.1) {
                    spike_view_shot = true;
                    spike_shots.push_back("convert-view");
                }
                if (spike_view_shot && !spike_pass && elapsed_s - spike_view_s >= 0.3) {
                    spike_pass = true;
                    spike_view_until = elapsed_s + 20.0;
                    line("spike: S1 PASS %.2f %u (visible %u, peak RSS %s, viewing 20 s)",
                         spike->seconds(), spike->splats(), renderer->last_visible_count(),
                         splat::bench::human_bytes(splat::bench::peak_rss_bytes()).c_str());
                }
            }

            ui_host->frame(input, view.midpoint);
            const splat::Status overlay = renderer->set_overlay_quads(ui_host->quads());
            if (!overlay.ok()) {
                line("viewer: FAIL ui overlay: %s", overlay.message().c_str());
                status = 9;
                reason = "FAIL ui overlay";
                break;
            }

            // ---- what the panel asked for ---------------------------------
            std::optional<splat::UiCommand> command;
            while (ui_app != nullptr && (command = ui_app->take_command()).has_value()) {
                switch (command->kind) {
                    case splat::UiCommand::ViewScene: {
                        // W19: the model-load phase is the one window a scene
                        // load waits for (W18 run B: inference leaves room for
                        // a 0.28 GiB scene upload next to the resident model).
                        if (ui_jobs != nullptr && !viewer::scene_loading_allowed(*ui_jobs)) {
                            ui_app->toast("Converting... scene switch in a moment");
                            line("viewer: jobs: ViewScene waits for the model load (album %s, "
                                 "index %d)",
                                 command->album.empty() ? "(loose)" : command->album.c_str(),
                                 command->index);
                            break;
                        }
                        // The album's scenes in listing order (the grid's
                        // focus index is the playlist index), through the same
                        // gallery the screen listed; the load itself is
                        // show_scene() above.
                        if (const char* why = show_scene(command->album, command->index)) {
                            ui_app->toast(why);
                        }
                        break;
                    }
                    case splat::UiCommand::ResumeScene:
                        // Circle at the panel's root: back to the scene that
                        // is still loaded (the panel stays where it was).
                        ui_host->set_visible(false);
                        break;
                    case splat::UiCommand::Quit:
                        // The one quit route a title has: the same LoadExec
                        // path as SECONDS and the quit file (below).
                        line("viewer: quit from the menu");
                        menu_quit = true;
                        break;
                }
            }
            if (menu_quit) break;
        }

        const splat::Status rendered = renderer->render(view, target, false);
        if (!rendered.ok()) {
            line("viewer: FAIL render: %s", rendered.message().c_str());
            run.gpu_ok = false;
            status = 7;
            reason = "FAIL render";
            break;
        }
        // The image is TRANSFER_SRC_OPTIMAL now; it stays that way into the
        // next frame's initial_layout.
        target.initial_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

        const splat::PassTimings& timings = renderer->timings();
        pass_ms += timings.project_ms + timings.sort_ms + timings.draw_ms;
        project_second_ms += timings.project_ms;
        sort_second_ms += timings.sort_ms;
        draw_second_ms += timings.draw_ms;

        const double copy_started = now_ms();
        if (!readback.pass.copy(target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                readback.buffer.buffer, extent, 2000ull * 1000 * 1000, error)) {
            line("viewer: FAIL frame copy: %s", error.c_str());
            run.gpu_ok = false;
            status = 7;
            reason = "FAIL frame copy";
            break;
        }
        const double copy_ms = now_ms() - copy_started;
        const double present_started = now_ms();
        double tile_ms = 0.0;
        if (!display.present(readback.buffer.mapped, &tile_ms, error)) {
            line("viewer: FAIL videoout present: %s", error.c_str());
            status = 8;
            reason = "FAIL videoout present";
            break;
        }
        const double present_wall_ms = now_ms() - present_started;
        if (!error.empty()) {
            line("viewer: present: %s", error.c_str());
            error.clear();
        }
        tile_total_ms += tile_ms;
        present_wall_total_ms += present_wall_ms;
        tile_second_ms += tile_ms;
        present_second_ms += present_wall_ms;
        copy_total_ms += copy_ms;
        copy_second_ms += copy_ms;
        ++frames;
        ++frames_this_second;

        // ---- shots and quit (UI=1) ------------------------------------------
        // The copy above is the finished frame: splats, comfort pass and the
        // panel overlay. The TV target is B8G8R8A8_SRGB, so those bytes are
        // B, G, R, A; PNG wants R, G, B and an opaque frame. `shots` is the
        // script's own list (W8's `shot <name>` lines); `spike_shots` is the
        // spike's programmatic request_shot names.
        if (!shots.empty() || !spike_shots.empty()) {
            const uint32_t shot_width = extent.width;
            const uint32_t shot_height = extent.height;
            const size_t shot_pixels = static_cast<size_t>(shot_width) * shot_height;
            const auto* bgra = static_cast<const uint8_t*>(readback.buffer.mapped);
            shot_rgba.resize(shot_pixels * 4);
            for (size_t i = 0; i < shot_pixels; ++i) {
                shot_rgba[i * 4 + 0] = bgra[i * 4 + 2];
                shot_rgba[i * 4 + 1] = bgra[i * 4 + 1];
                shot_rgba[i * 4 + 2] = bgra[i * 4 + 0];
                shot_rgba[i * 4 + 3] = 255;
            }
            auto write_shot = [&](const std::string& name, const char* kind) {
                const auto png = splat::encode_png(shot_rgba.data(),
                                                   static_cast<int>(shot_width),
                                                   static_cast<int>(shot_height));
                const std::string path = options.shots_dir + "/" + name + ".png";
                size_t bytes = 0;
                bool written = false;
                if (png.ok()) {
                    bytes = png.value().size();
                    std::FILE* file = std::fopen(path.c_str(), "wb");
                    if (file != nullptr) {
                        written = std::fwrite(png.value().data(), 1, bytes, file) == bytes;
                        std::fclose(file);
                    }
                }
                if (!written) {
                    line("viewer: FAIL shot %s: %s", name.c_str(),
                         png.ok() ? path.c_str() : png.message().c_str());
                    return false;
                }
                line("viewer: %s shot %s %zu bytes", kind, name.c_str(), bytes);
                return true;
            };
            for (const std::string& name : shots) {
                if (!write_shot(name, "script")) {
                    status = 9;
                    reason = "FAIL shot";
                    break;
                }
            }
            if (status == 0) {
                for (const std::string& name : spike_shots) {
                    if (!write_shot(name, "spike")) {
                        status = 9;
                        reason = "FAIL shot";
                        break;
                    }
                }
            }
            spike_shots.clear();
        }
        if (status != 0) break;
        if (spike_fail) {
            status = 10;
            reason = "FAIL spike";
            break;
        }
        if (spike_pass && elapsed_s >= spike_view_until) {
            // (e) 20 s of viewing the converted scene, then the usual end.
            line("viewer: spike: viewed %s for 20 s; ending", spike_out_name.c_str());
            break;
        }
        if (script_quit) {
            // The run ends through the same teardown/LoadExec path as SECONDS;
            // ui-test.sh waits for this line before pulling the PNGs.
            line("viewer: script done");
            break;
        }

        if (frames == 1) {
            line("viewer: first frame %.1f ms after the scene load started (%u visible splats)",
                 now_ms() - scene_started, renderer->last_visible_count());
        }

        const double now = now_ms();
        if (now - second_started >= 1000.0) {
            const double seconds = (now - second_started) / 1000.0;
            line("viewer: t=%.1fs fps=%.1f project=%.2f sort=%.2f draw=%.2f ms copy=%.2f tiles=%.2f "
                 "flip=%.2f ms visible=%u fade=%.3f head_x=%+.3f m",
                 (now - loop_started) / 1000.0, frames_this_second / seconds,
                 project_second_ms / frames_this_second, sort_second_ms / frames_this_second,
                 draw_second_ms / frames_this_second, copy_second_ms / frames_this_second,
                 tile_second_ms / frames_this_second,
                 (present_second_ms - tile_second_ms) / frames_this_second,
                 renderer->last_visible_count(), last_fade, last_head_x);
            frames_this_second = 0;
            tile_second_ms = 0.0;
            present_second_ms = 0.0;
            copy_second_ms = 0.0;
            project_second_ms = 0.0;
            sort_second_ms = 0.0;
            draw_second_ms = 0.0;
            second_started = now;
        }
    }

    const double run_s = (now_ms() - loop_started) / 1000.0;
    if (frames > 0) {
        line("viewer: totals: %llu frames in %.1f s (%.1f fps), GPU %.2f ms/frame, copy %.2f "
             "ms/frame, tiles %.2f ms/frame, present %.2f ms/frame",
             static_cast<unsigned long long>(frames), run_s, frames / run_s, pass_ms / frames,
             copy_total_ms / frames, tile_total_ms / frames, present_wall_total_ms / frames);
    } else {
        line("viewer: totals: no frame was presented");
    }

    // ---- the spike's verdict, if the run ended before one ------------------
    // A worker still inside inference is never destroyed under a GPU that may
    // still be reading its buffers: it is detached and the process exit
    // reclaims it (the same leak-on-timeout rule as the renderer's teardown).
    if (spike != nullptr) {
        if (spike_thread.joinable()) {
            line("viewer: spike: the converter was still running (phase %d); leaving its thread "
                 "and device to the process exit",
                 spike->phase());
            spike_thread.detach();
        }
        if (!spike_pass && !spike_fail) {
            line("spike: S1 FAIL the run ended after %.1f s without a verdict", run_s);
            if (status == 0) {
                status = 10;
                reason = "FAIL spike";
            }
        }
        log_pools();
    }

    // ---- a conversion still running at exit (W19) --------------------------
    // The job keeps its Running state in jobs.json (JobQueue::open re-queues it
    // next start), its worker is never joined and the renderer's device is not
    // freed under it: teardown() then leaks the GPU side, while the display is
    // still closed above (kernel state) before LoadExec - the same rule W18's
    // spike used for a worker that outlived its window.
    if (ui_jobs != nullptr && (viewer::jobs_active(*ui_jobs) || ui_jobs->busy())) {
        jobs_running = true;
        line("viewer: jobs: a conversion is still running at exit; leaving its worker and the model "
             "to the process exit (the queue re-queues the job next start)");
    }
    convert_gate.abort();

    // ---- teardown ---------------------------------------------------------
    teardown();

    end_title(status, reason);
}
