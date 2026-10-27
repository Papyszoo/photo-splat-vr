#pragma once

// Shared helpers for the SHARP stage 3-6 parity tests: locating the golden data
// (outside the repo, AGENTS.md), feeding a reference tensor into a stage as its
// input, and printing one comparable line per stage.
//
// The golden layout is docs/research/sharp-reference.md §10: one folder per
// photo under $SPLAT_DATA_DIR/golden with 030_depth_decoder_out.npy .. 090_*,
// plus reference.ply. Tests skip when the data or the GGUF is absent.

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif
#include <sys/resource.h>

#include "ggml.h"
#include "recon/ggml/graph.h"
#include "recon/sharp/sharp_model.h"
#include "recon/sharp/sharp_stage.h"
#include "../tools/parity/npy.h"
#include <nlohmann/json.hpp>

namespace splat {
namespace sharp_test {

namespace fs = std::filesystem;

inline std::string env_or_empty(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

struct Photo {
    std::string id;
    fs::path folder;
};

inline fs::path golden_root() { return env_or_empty("SPLAT_SHARP_GOLDEN"); }

// The golden photos under `root`, optionally narrowed with SPLAT_SHARP_PHOTOS
// (a substring match on the id). photo04_cropped is the 4:5 portrait crop and
// 90-degree rotation of photo01 that the F8 verification generated: non-square
// with a different aspect from every other photo, and without EXIF, so it
// exercises the composer's intrinsics and SHARP's 30 mm focal default on data
// the reference itself produced. It is only used when it exists.
inline std::vector<Photo> photos(const fs::path& root) {
    std::vector<Photo> found;
    const char* ids[] = {"photo01_living_room", "photo02_street", "photo03_reading_room",
                         "photo04_cropped"};
    const std::string filter = env_or_empty("SPLAT_SHARP_PHOTOS");
    for (const char* id : ids) {
        if (!filter.empty() && std::string(id).find(filter) == std::string::npos) continue;
        const fs::path folder = root / id;
        if (!fs::is_directory(folder)) continue;
        found.push_back(Photo{id, folder});
    }
    return found;
}

// The fp32 GGUF (fp32 gate) and the fp16 GGUF (the shipped runtime file).
inline fs::path gguf_path(bool fp16) {
    const fs::path path = env_or_empty(fp16 ? "SPLAT_SHARP_GGUF_F16" : "SPLAT_SHARP_GGUF");
    if (path.empty() || !fs::is_regular_file(path)) return {};
    return path;
}

// True when the case cannot run; prints why. Both the golden folder and the
// requested GGUF must exist.
inline bool skip_case(const char* name, bool fp16) {
    const fs::path golden = golden_root();
    if (golden.empty() || !fs::is_directory(golden)) {
        MESSAGE("skipping " << name << ": set SPLAT_SHARP_GOLDEN to the golden/ folder");
        return true;
    }
    if (gguf_path(fp16).empty()) {
        MESSAGE("skipping " << name << ": set "
                << (fp16 ? "SPLAT_SHARP_GGUF_F16" : "SPLAT_SHARP_GGUF") << " to the SHARP GGUF");
        return true;
    }
    return false;
}

struct Memory {
    size_t resident = 0;
    size_t peak = 0;
};

inline Memory process_memory() {
    Memory memory;
#if defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                  &count) == KERN_SUCCESS) {
        memory.resident = static_cast<size_t>(info.resident_size);
    }
#endif
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
#if defined(__APPLE__)
        memory.peak = static_cast<size_t>(usage.ru_maxrss);
#else
        memory.peak = static_cast<size_t>(usage.ru_maxrss) * 1024;
#endif
    }
    return memory;
}

inline std::string mib(size_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.0f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text;
}

inline void print_stats(const std::string& name, const parity::Stats& stats, const char* suffix) {
    if (!stats.ok) {
        MESSAGE("  " << name << ": " << stats.error);
        return;
    }
    char line[256];
    std::snprintf(line, sizeof(line),
                  "  %-28s n=%-9zu max_abs=%.3e max_rel=%.3e p99_rel=%.3e p99.9_rel=%.3e cos=%.7f%s",
                  name.c_str(), stats.compared, stats.max_abs, stats.max_rel, stats.p99_rel,
                  stats.p999_rel, stats.cosine, suffix);
    MESSAGE(line);
}

// The five encoder level maps, read from 021_encoder_out_XX.npy and uploaded
// into one session-owned arena, so a decoder case is isolated from stage 2.
// The session must outlive the tensors (it owns the arena).
struct LevelTensors {
    std::array<ggml_tensor*, 5> levels{{nullptr, nullptr, nullptr, nullptr, nullptr}};
};

inline bool upload_levels(GraphSession& session, GgmlWeights& weights, const Photo& photo,
                          LevelTensors* out, std::string* error) {
    static const char* kFiles[5] = {"021_encoder_out_00.npy", "021_encoder_out_01.npy",
                                    "021_encoder_out_02.npy", "021_encoder_out_03.npy",
                                    "021_encoder_out_04.npy"};
    static const int64_t kDim[5] = {768, 384, 192, 96, 48};
    static const int64_t kChannels[5] = {256, 256, 512, 1024, 1024};
    for (int index = 0; index < 5; ++index) {
        std::vector<int64_t> shape;
        std::vector<float> values;
        if (!parity::read_npy_f32((photo.folder / kFiles[index]).string(), &shape, &values, error)) {
            return false;
        }
        const size_t expected = static_cast<size_t>(kChannels[index]) * kDim[index] * kDim[index];
        if (values.size() != expected) {
            *error = std::string(kFiles[index]) + ": unexpected element count";
            return false;
        }
        char name[32];
        std::snprintf(name, sizeof(name), "golden_level_%02d", index);
        ggml_tensor* tensor =
            session.persistent(name, {kDim[index], kDim[index], kChannels[index], 1});
        const Status uploaded = weights.upload(tensor, values.data(), values.size() * sizeof(float));
        if (!uploaded.ok()) {
            *error = uploaded.message();
            return false;
        }
        out->levels[static_cast<size_t>(index)] = tensor;
    }
    return true;
}

// Writes a float32 .npy in the same C order the goldens use, so a failing stage
// can be diffed against a PyTorch re-run. Only used when SPLAT_SHARP_DUMP_DIR is
// set (debugging aid, never part of a gated run).
inline bool write_npy_f32(const std::string& path, const std::vector<int64_t>& ne,
                          const float* values, size_t count) {
    std::vector<int64_t> shape;
    for (size_t index = 0; index < ne.size(); ++index) shape.push_back(ne[ne.size() - 1 - index]);
    std::string header = "{'descr': '<f4', 'fortran_order': False, 'shape': (";
    for (size_t index = 0; index < shape.size(); ++index) {
        if (index != 0) header += ", ";
        header += std::to_string(shape[index]);
    }
    if (shape.size() == 1) header += ",";
    header += "), }";
    // numpy pads the header to a multiple of 64 bytes including the magic.
    size_t padded = header.size() + 1;
    while ((10 + padded) % 64 != 0) ++padded;
    header.append(padded - header.size() - 1, ' ');
    header += "\n";
    std::ofstream stream(path, std::ios::binary);
    if (!stream) return false;
    stream.write("\x93NUMPY", 6);
    stream.put('\x01');
    stream.put('\x00');
    const uint16_t length = static_cast<uint16_t>(header.size());
    stream.put(static_cast<char>(length & 0xff));
    stream.put(static_cast<char>(length >> 8));
    stream.write(header.data(), static_cast<std::streamsize>(header.size()));
    stream.write(reinterpret_cast<const char*>(values),
                 static_cast<std::streamsize>(count * sizeof(float)));
    return static_cast<bool>(stream);
}

inline void dump_stage(const std::string& name, const SharpStage& stage) {
    const std::string directory = env_or_empty("SPLAT_SHARP_DUMP_DIR");
    if (directory.empty()) return;
    std::filesystem::create_directories(directory);
    const std::string path = directory + "/port_" + name + ".npy";
    size_t count = 1;
    for (int64_t dimension : stage.shape) count *= static_cast<size_t>(dimension);
    if (stage.values.size() != count) return;
    write_npy_f32(path, stage.shape, stage.values.data(), count);
}

inline bool read_golden(const Photo& photo, const std::string& file, std::vector<float>* values,
                        std::string* error) {
    std::vector<int64_t> shape;
    if (!parity::read_npy_f32((photo.folder / file).string(), &shape, values, error)) return false;
    return true;
}

// 000_meta.json: the photo's own intrinsics. The reference's `focal_length_px`
// is what `cli/predict.py` uses (EXIF or SHARP's 30 mm default) and
// `disparity_factor = focal / width` is the network's conditioning input.
struct PhotoMeta {
    float focal_px = 0.0f;
    int width = 0;
    int height = 0;
    int layers = 2;
};

inline bool read_photo_meta(const Photo& photo, PhotoMeta* meta, std::string* error) {
    std::ifstream stream(photo.folder / "000_meta.json");
    if (!stream) {
        *error = "cannot open 000_meta.json for " + photo.id;
        return false;
    }
    nlohmann::json root;
    try {
        stream >> root;
        meta->focal_px = root.at("focal_length_px").get<float>();
        meta->width = root.at("image_size").at(0).get<int>();
        meta->height = root.at("image_size").at(1).get<int>();
        meta->layers = root.at("num_layers").get<int>();
    } catch (const std::exception& parse_error) {
        *error = std::string("000_meta.json: ") + parse_error.what();
        return false;
    }
    return true;
}

}  // namespace sharp_test
}  // namespace splat
