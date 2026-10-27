// splat_depth_scene: an external depth map + photo -> C4 scene folder.
//
//   splat_depth_scene --depth-map <map.npy> [--disparity|--relative-depth] [options]
//                     <photo> <out_scene_dir>
//
// The C4 fallback builds its scene from a full-resolution metric depth map
// (`fallback_scene_from_depth`); normally Depth Anything V2 inside this repo
// produces that map. This CLI feeds a map produced elsewhere (MoGe-2, Depth
// Anything 3, ...) into the same builder, so an outside model is compared on
// exactly the product's splat construction instead of a second one. Nothing
// here builds Gaussians: the map is converted to metres (when the model
// predicts relative inverse depth) and handed to the fallback.
//
// Options:
//   --depth-map <file.npy>  float32 depth map, C order, shape (H, W) matching
//                           the photo (upsample before calling)
//   --disparity             the map is relative inverse depth (what DA2
//                           predicts). The median lands on --scene-depth, like
//                           the fallback.
//   --relative-depth        the map is depth up to scale (DA3-Small's output:
//                           proportional to metres, with a small additive
//                           bias that this scale-only anchor does not remove).
//                           Same anchor: median -> --scene-depth.
//                           Without either flag the map is metres.
//   --scene-depth <m>       assumed scene depth for the two relative modes
//                           (default 3.0)
//   --focal <px>            focal length in photo pixels; default: EXIF, else
//                           SHARP's 30 mm (same helper as the job runner)
//   --splats <count>        target Gaussian count (default 500000)
//   --input-size <px>       network short side, used for the silhouette ramp
//                           width (default 518; pass the model's real input)
//   --backend <name>        name written to meta.json's "backend" (default
//                           "external-depth"); the in-headset label reads it
//   --cpu                   force the CPU backend for the splat grid
//
// The scene's meta.json intrinsics are what the builder used: fx = fy = focal,
// principal point at the image centre. Pass the model's own focal when it
// estimates one and its principal point is centred (MoGe-2, DA3).

#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <sys/resource.h>
#include <unistd.h>

#include "common/bytes.h"
#include "common/image.h"
#include "common/path.h"
#include "jobs/image_io.h"
#include "recon/fallback/fallback_reconstructor.h"
#include "scene/scene_folder.h"

namespace {

namespace fs = std::filesystem;

uint64_t peak_rss_bytes() {
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#if defined(__APPLE__)
    return static_cast<uint64_t>(usage.ru_maxrss);
#else
    return static_cast<uint64_t>(usage.ru_maxrss) * 1024;
#endif
}

void usage() {
    std::fprintf(stderr,
                 "usage: splat_depth_scene --depth-map <map.npy>\n"
                 "                         [--disparity|--relative-depth] [--scene-depth <m>]\n"
                 "                         [--focal <px>] [--splats <count>]\n"
                 "                         [--input-size <px>] [--backend <name>] [--cpu]\n"
                 "                         <photo> <out_scene_dir>\n");
}

// Reads a float32 (H, W) .npy written by numpy.save; anything else is an error
// rather than a guess. Little-endian C-order is what numpy writes on this host
// and what the model harness saves.
bool read_npy_float32(const std::string& path, std::vector<float>* values, int* width,
                      int* height, std::string* error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        *error = "cannot open " + path;
        return false;
    }
    char magic[6] = {};
    stream.read(magic, 6);
    if (std::memcmp(magic, "\x93NUMPY", 6) != 0) {
        *error = path + ": not a .npy file";
        return false;
    }
    unsigned char version[2] = {};
    stream.read(reinterpret_cast<char*>(version), 2);
    uint32_t header_length = 0;
    if (version[0] == 1) {
        unsigned char raw[2] = {};
        stream.read(reinterpret_cast<char*>(raw), 2);
        header_length = static_cast<uint32_t>(raw[0]) | (static_cast<uint32_t>(raw[1]) << 8);
    } else {
        unsigned char raw[4] = {};
        stream.read(reinterpret_cast<char*>(raw), 4);
        header_length = static_cast<uint32_t>(raw[0]) | (static_cast<uint32_t>(raw[1]) << 8) |
                        (static_cast<uint32_t>(raw[2]) << 16) |
                        (static_cast<uint32_t>(raw[3]) << 24);
    }
    std::string header(header_length, '\0');
    stream.read(header.data(), header_length);
    if (header.find("'<f4'") == std::string::npos) {
        *error = path + ": only little-endian float32 maps are read";
        return false;
    }
    if (header.find("'fortran_order': True") != std::string::npos) {
        *error = path + ": Fortran order is not read; save with numpy.save on a C-order array";
        return false;
    }
    const size_t shape_at = header.find("'shape': (");
    const size_t close = shape_at == std::string::npos ? std::string::npos
                                                       : header.find(')', shape_at);
    if (shape_at == std::string::npos || close == std::string::npos) {
        *error = path + ": no shape in the header";
        return false;
    }
    const std::string shape_text = header.substr(shape_at + 10, close - shape_at - 10);
    std::vector<long long> shape;
    std::string number;
    for (char character : shape_text) {
        if (character == ',' || character == ' ') {
            if (!number.empty()) {
                shape.push_back(std::stoll(number));
                number.clear();
            }
        } else if (character >= '0' && character <= '9') {
            number.push_back(character);
        } else {
            *error = path + ": malformed shape in the header";
            return false;
        }
    }
    if (!number.empty()) shape.push_back(std::stoll(number));
    if (shape.size() != 2 || shape[0] <= 0 || shape[1] <= 0) {
        *error = path + ": the depth map must be a (H, W) array";
        return false;
    }
    *height = static_cast<int>(shape[0]);
    *width = static_cast<int>(shape[1]);
    values->resize(static_cast<size_t>(*width) * static_cast<size_t>(*height));
    stream.read(reinterpret_cast<char*>(values->data()),
                static_cast<std::streamsize>(values->size() * sizeof(float)));
    if (!stream) {
        *error = path + ": truncated payload";
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string depth_map;
    std::string photo;
    std::string output;
    std::string backend = "external-depth";
    enum class Scale { kMetres, kDisparity, kRelativeDepth };
    Scale scale = Scale::kMetres;
    bool have_focal = false;
    float focal = 0.0f;
    splat::FallbackConfig config;
    config.input_size = 518;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto next = [&](const char* name) -> std::string {
            if (index + 1 >= argc) {
                std::fprintf(stderr, "splat_depth_scene: %s needs a value\n", name);
                std::exit(2);
            }
            return argv[++index];
        };
        if (argument == "-h" || argument == "--help") {
            usage();
            return 0;
        } else if (argument == "--depth-map") {
            depth_map = next("--depth-map");
        } else if (argument == "--disparity") {
            scale = Scale::kDisparity;
        } else if (argument == "--relative-depth") {
            scale = Scale::kRelativeDepth;
        } else if (argument == "--scene-depth") {
            config.assumed_scene_depth_m = std::stof(next("--scene-depth"));
        } else if (argument == "--focal") {
            focal = std::stof(next("--focal"));
            have_focal = focal > 0.0f;
            if (!have_focal) {
                std::fprintf(stderr, "splat_depth_scene: --focal must be positive\n");
                return 2;
            }
        } else if (argument == "--splats") {
            config.target_splats = static_cast<size_t>(std::stoul(next("--splats")));
        } else if (argument == "--input-size") {
            config.input_size = std::stoi(next("--input-size"));
        } else if (argument == "--backend") {
            backend = next("--backend");
        } else if (argument == "--cpu") {
            config.prefer_vulkan = false;
        } else if (photo.empty()) {
            photo = argument;
        } else if (output.empty()) {
            output = argument;
        } else {
            std::fprintf(stderr, "splat_depth_scene: unexpected argument '%s'\n", argument.c_str());
            usage();
            return 2;
        }
    }
    if (depth_map.empty() || photo.empty() || output.empty()) {
        usage();
        return 2;
    }
    if (backend.empty()) {
        std::fprintf(stderr, "splat_depth_scene: --backend needs a name\n");
        return 2;
    }
    if (config.input_size <= 0) {
        std::fprintf(stderr, "splat_depth_scene: --input-size must be positive\n");
        return 2;
    }

    const fs::path out_dir = fs::absolute(output);
    if (fs::exists(out_dir) && !fs::is_empty(out_dir)) {
        std::fprintf(stderr, "splat_depth_scene: %s already exists and is not empty\n",
                     out_dir.string().c_str());
        return 2;
    }

    splat::Result<std::vector<uint8_t>> file_bytes = splat::read_file_bytes(photo);
    if (!file_bytes.ok()) {
        std::fprintf(stderr, "splat_depth_scene: %s\n", file_bytes.message().c_str());
        return 1;
    }
    splat::Result<splat::Image> decoded = splat::decode_image(file_bytes.value().data(),
                                                              file_bytes.value().size());
    if (!decoded.ok()) {
        std::fprintf(stderr, "splat_depth_scene: cannot decode %s: %s\n", photo.c_str(),
                     decoded.message().c_str());
        return 1;
    }
    splat::Image image = decoded.take();
    const int width = image.width;
    const int height = image.height;

    std::vector<float> values;
    int map_width = 0;
    int map_height = 0;
    std::string error;
    if (!read_npy_float32(depth_map, &values, &map_width, &map_height, &error)) {
        std::fprintf(stderr, "splat_depth_scene: %s\n", error.c_str());
        return 1;
    }
    if (map_width != width || map_height != height) {
        std::fprintf(stderr,
                     "splat_depth_scene: depth map is %dx%d but the photo is %dx%d "
                     "(upsample the map to the photo before calling)\n",
                     map_width, map_height, width, height);
        return 1;
    }

    if (!have_focal) {
        const splat::Result<float> from_exif =
            splat::focal_px_from_photo(file_bytes.value().data(), file_bytes.value().size(), width,
                                       height);
        if (!from_exif.ok()) {
            std::fprintf(stderr, "splat_depth_scene: cannot compute focal length: %s\n",
                         from_exif.message().c_str());
            return 1;
        }
        focal = from_exif.value();
    }

    std::vector<float> depth;
    const char* scale_name = "metres";
    switch (scale) {
        case Scale::kDisparity:
            depth = splat::fallback_metric_depth_from_disparity(values, config);
            scale_name = "relative inverse depth";
            break;
        case Scale::kRelativeDepth:
            depth = splat::fallback_metric_depth_from_relative_depth(values, config);
            scale_name = "depth up to scale";
            break;
        case Scale::kMetres:
            depth = splat::fallback_metric_depth_from_metres(values, config);
            break;
    }

    splat::Result<splat::Image> thumbnail = splat::make_thumbnail(image);
    if (!thumbnail.ok()) {
        std::fprintf(stderr, "splat_depth_scene: cannot build thumbnail: %s\n",
                     thumbnail.message().c_str());
        return 1;
    }
    splat::Result<std::vector<uint8_t>> thumbnail_jpeg = splat::encode_jpeg(thumbnail.value());
    if (!thumbnail_jpeg.ok()) {
        std::fprintf(stderr, "splat_depth_scene: cannot encode thumbnail: %s\n",
                     thumbnail_jpeg.message().c_str());
        return 1;
    }
    std::vector<uint8_t> original_jpeg;
    if (splat::looks_like_jpeg(file_bytes.value().data(), file_bytes.value().size())) {
        original_jpeg = file_bytes.take();
    } else {
        splat::Result<std::vector<uint8_t>> encoded = splat::encode_jpeg(image);
        if (!encoded.ok()) {
            std::fprintf(stderr, "splat_depth_scene: cannot encode original: %s\n",
                         encoded.message().c_str());
            return 1;
        }
        original_jpeg = encoded.take();
    }

    std::fprintf(stderr, "depth-map %s (%s, %dx%d)\n", depth_map.c_str(), scale_name, map_width,
                 map_height);
    std::fprintf(stderr, "photo     %s (%dx%d)\n", photo.c_str(), width, height);
    std::fprintf(stderr, "focal     %.2f px%s\n", focal, have_focal ? " (given)" : " (EXIF/30 mm)");
    std::fprintf(stderr, "splats    target %zu\n", config.target_splats);

    std::string stage;
    double stage_started = 0.0;
    const auto clock_started = std::chrono::steady_clock::now();
    auto seconds_now = [&clock_started]() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - clock_started)
            .count();
    };
    int last_decile = -1;
    splat::Progress progress([&](float fraction, const std::string& label) {
        if (label != stage) {
            if (!stage.empty()) {
                std::printf("stage    %-20s %.3f s\n", stage.c_str(),
                            seconds_now() - stage_started);
            }
            stage = label;
            stage_started = seconds_now();
        }
        const int decile = static_cast<int>(fraction * 10.0f);
        if (decile > last_decile) {
            last_decile = decile;
            std::fprintf(stderr, "  %3d%% %s\n", decile * 10, label.c_str());
        }
    });

    splat::FallbackFillStats stats;
    splat::Result<splat::SplatScene> scene =
        splat::fallback_scene_from_depth(depth, image, focal, config,
                                         splat::kFallbackDisocclusionHeadMotionM, progress, &stats);
    if (!scene.ok()) {
        std::fprintf(stderr, "splat_depth_scene: %s\n", scene.message().c_str());
        return 1;
    }
    if (!scene.value().consistent()) {
        std::fprintf(stderr, "splat_depth_scene: the builder returned an inconsistent scene\n");
        return 1;
    }

    splat::SceneMeta meta;
    meta.backend = backend;
    meta.intrinsics.width = static_cast<uint32_t>(width);
    meta.intrinsics.height = static_cast<uint32_t>(height);
    meta.intrinsics.fx = focal;
    meta.intrinsics.fy = focal;
    meta.intrinsics.cx = static_cast<float>(width) / 2.0f;
    meta.intrinsics.cy = static_cast<float>(height) / 2.0f;
    meta.seconds_taken = seconds_now();

    const splat::Status written =
        splat::write_scene_folder(out_dir, scene.value(), meta, original_jpeg,
                                  thumbnail_jpeg.value());
    if (!written.ok()) {
        std::fprintf(stderr, "splat_depth_scene: cannot write %s: %s\n", out_dir.string().c_str(),
                     written.message().c_str());
        return 1;
    }

    std::printf("scene    %s\n", out_dir.string().c_str());
    std::printf("backend  %s\n", backend.c_str());
    std::printf("splats   %zu\n", scene.value().count());
    std::printf("meta     fx %.2f fy %.2f cx %.1f cy %.1f\n", meta.intrinsics.fx,
                meta.intrinsics.fy, meta.intrinsics.cx, meta.intrinsics.cy);
    std::printf("fill     %zu samples, %zu ghosts\n", stats.fill_samples, stats.ghost_samples);
    std::printf("seconds  %.3f\n", meta.seconds_taken);
    std::printf("peak rss %s (%" PRIu64 " bytes)\n",
                splat::human_bytes(peak_rss_bytes()).c_str(), peak_rss_bytes());
    return 0;
}
