// Per-stage parity of the C3 SHARP port (stages 1-2: preprocess + Depth Pro
// encoder) against the PyTorch reference dumps in golden/photo01..03.
//
// The data lives outside the repo (AGENTS.md), so every case skips when it is
// missing:
//
//   export SPLAT_SHARP_GOLDEN=/path/to/golden            # photo01..03 folders
//   export SPLAT_SHARP_GGUF=/path/to/sharp-...-f32-v1.gguf   # fp32 parity target
//   export SPLAT_SHARP_GGUF_F16=/path/to/sharp-...-f16-v1.gguf
//   export SPLAT_SHARP_CHECKPOINT=/path/to/sharp_2572gikvuh.pt  # only to build the fp32 file
//   export SPLAT_SHARP_PHOTOS=photo02_street             # optional, narrows the run
//   build-F8-sharp/tests/splat_tests -tc="sharp*"
//
// The fp32 GGUF is what the fp32 gate needs; `splat_convert --fp32` produces it
// (core/model/converter.cpp, tests/sharp_converter_fp32_test.cpp). This test
// reuses the file if it is already next to the fp16 one and otherwise
// materialises a parity copy from the checkpoint (same tensor table and
// metadata, float32 payloads), so the gate runs on any machine that has the
// checkpoint.
//
// Layout: .npy dumps are torch NCHW / (B, N, C); ggml tensors are ne order. Both
// are row-major over the same linear element order, so the comparisons are
// element-wise after a shape check (tools/parity/npy.h).

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include "../tools/parity/npy.h"
#include "gguf.h"
#include "jobs/image_io.h"
#include "model/checkpoint.h"
#include "model/gguf_writer.h"
#include "recon/ggml/backend.h"
#include "recon/sharp/sharp_encoder.h"
#include "recon/sharp/sharp_preprocess.h"

#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/resource.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

std::string env_or_empty(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

// ---- process memory ---------------------------------------------------------

struct Memory {
    size_t resident = 0;  // current RSS
    size_t peak = 0;      // high-water mark for the process
};

Memory process_memory() {
    Memory memory;
#if defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                  &count) == KERN_SUCCESS) {
        memory.resident = static_cast<size_t>(info.resident_size);
    }
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        memory.peak = static_cast<size_t>(usage.ru_maxrss);  // bytes on macOS
    }
#else
    std::ifstream statm("/proc/self/statm");
    long pages = 0;
    long resident_pages = 0;
    if (statm >> pages >> resident_pages) {
        memory.resident = static_cast<size_t>(resident_pages) * static_cast<size_t>(::sysconf(_SC_PAGESIZE));
    }
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        memory.peak = static_cast<size_t>(usage.ru_maxrss) * 1024;  // KiB on Linux
    }
#endif
    return memory;
}

std::string mib(size_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.0f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text;
}

// ---- fp32 parity GGUF -------------------------------------------------------
//
// `splat_convert --fp32` used to ignore the flag in the SHARP profile (it is now
// fixed, see sharp_converter_fp32_test.cpp), so an older fp32 GGUF may be
// absent. This materialises one next to the fp16 file when it is missing (same
// tensor table and metadata, float32 payloads read from the checkpoint). It is a
// parity artifact, never shipped.

bool materialise_fp32_gguf(const fs::path& fp16_path, const fs::path& checkpoint_path,
                           const fs::path& output, std::string* error) {
    ggml_context* meta = nullptr;
    gguf_init_params params{};
    params.no_alloc = true;
    params.ctx = &meta;
    gguf_context* gguf = gguf_init_from_file(fp16_path.string().c_str(), params);
    if (gguf == nullptr) {
        *error = "cannot read " + fp16_path.string();
        return false;
    }
    // gguf_init_from_file allocates the metadata context through params.ctx; it
    // is the caller's to free, and it is only needed for the tensor table below.
    struct MetaGuard {
        ggml_context*& context;
        ~MetaGuard() { ggml_free(context); }
    } meta_guard{meta};
    const int64_t tensor_count = gguf_get_n_tensors(gguf);
    std::vector<splat::GgufTensorDesc> descriptions(static_cast<size_t>(tensor_count));
    for (int64_t index = 0; index < tensor_count; ++index) {
        const ggml_tensor* tensor = ggml_get_tensor(meta, gguf_get_tensor_name(gguf, index));
        if (tensor == nullptr) {
            gguf_free(gguf);
            *error = "GGUF tensor table is inconsistent";
            return false;
        }
        splat::GgufTensorDesc description;
        description.name = gguf_get_tensor_name(gguf, index);
        for (int axis = static_cast<int>(ggml_n_dims(tensor)) - 1; axis >= 0; --axis) {
            description.shape.push_back(tensor->ne[axis]);
        }
        description.fp16 = false;
        descriptions[static_cast<size_t>(index)] = std::move(description);
    }
    const int64_t names_key = gguf_find_key(gguf, "sharp.source_tensor_names");
    if (names_key < 0 || gguf_get_arr_n(gguf, names_key) != static_cast<size_t>(tensor_count)) {
        gguf_free(gguf);
        *error = fp16_path.string() + " has no sharp.source_tensor_names matching its tensors";
        return false;
    }
    std::vector<std::string> torch_names(static_cast<size_t>(tensor_count));
    for (int64_t index = 0; index < tensor_count; ++index) {
        torch_names[static_cast<size_t>(index)] = gguf_get_arr_str(gguf, names_key, static_cast<size_t>(index));
    }

    splat::Result<splat::Checkpoint> checkpoint = splat::open_checkpoint(checkpoint_path);
    if (!checkpoint.ok()) {
        gguf_free(gguf);
        *error = checkpoint.message();
        return false;
    }
    std::map<std::string, const splat::CheckpointTensor*> by_name;
    for (const splat::CheckpointTensor& tensor : checkpoint.value().tensors) {
        by_name[tensor.record.name] = &tensor;
    }

    splat::GgufWriter writer;
    splat::Status status = writer.begin(output, descriptions);
    if (!status.ok()) {
        gguf_free(gguf);
        *error = status.message();
        return false;
    }
    for (int64_t key = 0; key < gguf_get_n_kv(gguf); ++key) {
        const std::string name = gguf_get_key(gguf, key);
        if (name == "sharp.converter" || name == "sharp.changes") continue;
        switch (gguf_get_kv_type(gguf, key)) {
            case GGUF_TYPE_STRING:
                writer.set_string(name, gguf_get_val_str(gguf, key));
                break;
            case GGUF_TYPE_UINT32:
                writer.set_u32(name, gguf_get_val_u32(gguf, key));
                break;
            case GGUF_TYPE_UINT64:
                writer.set_u64(name, gguf_get_val_u64(gguf, key));
                break;
            case GGUF_TYPE_ARRAY: {
                std::vector<std::string> values;
                for (size_t item = 0; item < gguf_get_arr_n(gguf, key); ++item) {
                    values.emplace_back(gguf_get_arr_str(gguf, key, item));
                }
                writer.set_strings(name, values);
                break;
            }
            default:
                gguf_free(gguf);
                *error = "unexpected metadata type for '" + name + "'";
                return false;
        }
    }
    writer.set_string("sharp.converter", "parity-fp32");
    writer.set_strings("sharp.changes",
                       {"dtype: every tensor kept as float32 (parity copy of the fp16 GGUF's tensor table)",
                        "purpose: fp32 stage parity for the C3 tests; never shipped",
                        "layout: torch shape (d0..dn) stored as ggml ne (dn..d0); no transpose applied"});
    gguf_free(gguf);

    status = writer.write_header();
    if (!status.ok()) {
        *error = status.message();
        return false;
    }
    for (int64_t index = 0; index < tensor_count; ++index) {
        const auto found = by_name.find(torch_names[static_cast<size_t>(index)]);
        if (found == by_name.end()) {
            *error = "checkpoint is missing '" + torch_names[static_cast<size_t>(index)] + "'";
            return false;
        }
        uint64_t bytes = 0;
        status = writer.begin_tensor(&bytes);
        if (!status.ok()) {
            *error = status.message();
            return false;
        }
        status = splat::read_checkpoint_tensor(
            checkpoint.value().zip, *found->second,
            [&](uint64_t, const void* data, size_t size) {
                return writer.append_tensor_bytes(data, size).ok();
            });
        if (!status.ok()) {
            *error = status.message();
            return false;
        }
        status = writer.end_tensor();
        if (!status.ok()) {
            *error = status.message();
            return false;
        }
    }
    status = writer.finish();
    if (!status.ok()) {
        *error = status.message();
        return false;
    }
    return true;
}

// Returns the fp32 GGUF to use, materialising it when the sibling file is
// missing. Empty when the inputs are not available.
fs::path ensure_fp32_gguf(std::string* error) {
    const fs::path fp16 = env_or_empty("SPLAT_SHARP_GGUF_F16");
    const fs::path wanted = env_or_empty("SPLAT_SHARP_GGUF");
    if (!wanted.empty() && fs::is_regular_file(wanted)) return wanted;
    if (fp16.empty() || !fs::is_regular_file(fp16)) return {};
    fs::path checkpoint = env_or_empty("SPLAT_SHARP_CHECKPOINT");
    if (checkpoint.empty()) checkpoint = fp16.parent_path() / splat::kSharpCheckpointName;
    if (!fs::is_regular_file(checkpoint)) return {};
    std::string name = fp16.stem().string();
    const std::string from = "f16";
    const size_t at = name.rfind(from);
    if (at != std::string::npos) name.replace(at, from.size(), "f32");
    const fs::path output = wanted.empty() ? fp16.parent_path() / (name + ".gguf") : wanted;
    if (fs::is_regular_file(output)) return output;
    MESSAGE("materialising " << output << " from " << checkpoint << " (fp32 parity weights)");
    if (!materialise_fp32_gguf(fp16, checkpoint, output, error)) return {};
    return output;
}

// ---- golden photos ----------------------------------------------------------

struct GoldenPhoto {
    std::string id;
    fs::path folder;
    fs::path jpeg;
};

// photo01..03, optionally narrowed to the ids in SPLAT_SHARP_PHOTOS.
// photo04_cropped (tests/sharp_golden.h) is deliberately not in this list: this
// file tests stage 2, which the three photos already cover, and a CPU fp32 case
// costs ~110 s per photo.
std::vector<GoldenPhoto> golden_photos(const fs::path& root) {
    std::vector<GoldenPhoto> photos;
    const char* ids[] = {"photo01_living_room", "photo02_street", "photo03_reading_room"};
    const std::string filter = env_or_empty("SPLAT_SHARP_PHOTOS");
    for (const char* id : ids) {
        if (!filter.empty() && std::string(id).find(filter) == std::string::npos) continue;
        const fs::path folder = root / id;
        if (!fs::is_directory(folder)) continue;
        GoldenPhoto photo;
        photo.id = id;
        photo.folder = folder;
        photo.jpeg = root / "inputs" / (std::string(id) + ".jpg");
        photos.push_back(std::move(photo));
    }
    return photos;
}

// ---- stage expectations -----------------------------------------------------

struct StageExpectation {
    std::string stage;
    std::string file;
};

const StageExpectation kEncoderStages[] = {
    {"encoder_out_00", "021_encoder_out_00.npy"},
    {"encoder_out_01", "021_encoder_out_01.npy"},
    {"encoder_out_02", "021_encoder_out_02.npy"},
    {"encoder_out_03", "021_encoder_out_03.npy"},
    {"encoder_out_04", "021_encoder_out_04.npy"},
};

void print_stats(const std::string& name, const splat::parity::Stats& stats, const char* suffix) {
    if (!stats.ok) {
        MESSAGE("  " << name << ": " << stats.error);
        return;
    }
    char line[256];
    std::snprintf(line, sizeof(line),
                  "  %-14s n=%-9zu max_abs=%.3e max_rel=%.3e p99_rel=%.3e p99.9_rel=%.3e cos=%.7f%s",
                  name.c_str(), stats.compared, stats.max_abs, stats.max_rel, stats.p99_rel,
                  stats.p999_rel, stats.cosine, suffix);
    MESSAGE(line);
}

// Decodes the reference JPEG and runs stage 0. Returns the normalised encoder
// input (CHW, [-1,1]) on success.
//
// The reference pipeline decodes with PIL and this build decodes with stb
// (core/jobs/image_io); the two decoders disagree by up to 3/255 on ~6% of the
// bytes at colour edges, because stb's chroma upsampling is nearest-neighbour
// where libjpeg's is bilinear. That is a decoder difference, not a
// preprocessing one, so the gate feeds the reference's own uint8 pixels
// (010_image_uint8.npy) through sharp_preprocess and the difference from our
// decode is reported separately.
bool run_preprocess(const GoldenPhoto& photo, std::vector<float>* normalised, int* size,
                    std::string* error) {
    splat::Result<std::vector<uint8_t>> bytes = splat::read_file_bytes(photo.jpeg);
    if (!bytes.ok()) {
        *error = bytes.message();
        return false;
    }
    splat::Result<splat::Image> image = splat::decode_image(bytes.value().data(), bytes.value().size());
    if (!image.ok()) {
        *error = image.message();
        return false;
    }

    std::vector<int64_t> reference_shape;
    std::vector<uint8_t> reference;
    if (!splat::parity::read_npy_u8((photo.folder / "010_image_uint8.npy").string(), &reference_shape,
                                    &reference, error)) {
        return false;
    }
    if (reference_shape.size() != 3 || static_cast<int>(reference_shape[0]) != image.value().height ||
        static_cast<int>(reference_shape[1]) != image.value().width) {
        *error = "decoded image size does not match 010_image_uint8.npy";
        return false;
    }
    size_t differing = 0;
    int largest = 0;
    for (size_t index = 0; index < reference.size(); ++index) {
        const int delta = std::abs(static_cast<int>(reference[index]) - image.value().rgb[index]);
        if (delta != 0) ++differing;
        largest = std::max(largest, delta);
    }
    MESSAGE("  decode          PIL and stb agree on size (" << image.value().width << "x"
            << image.value().height << "); " << differing << " of " << reference.size()
            << " bytes differ (max " << largest << ", stb chroma upsampling)");

    const int width = static_cast<int>(reference_shape[1]);
    const int height = static_cast<int>(reference_shape[0]);

    // The stage under test, fed the reference's own pixels: bit-exact is the
    // expectation (float32 arithmetic identical to PyTorch's CPU kernel).
    splat::Result<splat::SharpImage> preprocessed =
        splat::sharp_preprocess(reference.data(), width, height);
    if (!preprocessed.ok()) {
        *error = preprocessed.message();
        return false;
    }
    const int image_size = preprocessed.value().size;
    const std::vector<int64_t> ne = {image_size, image_size, 3, 1};
    splat::parity::Stats stats = splat::parity::compare_npy(
        (photo.folder / "011_preprocess_out.npy").string(), ne, preprocessed.value().chw.data());
    print_stats("011 [0,1]", stats, "");
    if (!stats.ok) {
        *error = stats.error;
        return false;
    }
    CHECK_MESSAGE(stats.max_abs <= 1e-6, "preprocess resize max abs " << stats.max_abs);
    CHECK_MESSAGE(stats.cosine >= 0.9999999, "preprocess resize cosine " << stats.cosine);

    *normalised = preprocessed.value().chw;
    splat::sharp_normalise(normalised);
    splat::parity::Stats encoder_in = splat::parity::compare_npy(
        (photo.folder / "020_encoder_in.npy").string(), ne, normalised->data());
    print_stats("020 [-1,1]", encoder_in, "");
    if (!encoder_in.ok) {
        *error = encoder_in.error;
        return false;
    }
    CHECK_MESSAGE(encoder_in.max_abs <= 2e-6, "normalised input max abs " << encoder_in.max_abs);

    // Same stage on our own decode, reported only: the gap is the decoder.
    splat::Result<splat::SharpImage> own =
        splat::sharp_preprocess(image.value().rgb.data(), image.value().width, image.value().height);
    if (own.ok()) {
        splat::parity::Stats own_stats = splat::parity::compare_npy(
            (photo.folder / "011_preprocess_out.npy").string(), ne, own.value().chw.data());
        print_stats("011 [stb]", own_stats, " (decoder difference, not gated)");
    }
    *size = image_size;
    return true;
}

void run_encoder_case(bool prefer_vulkan, bool fp16) {
    const fs::path golden = env_or_empty("SPLAT_SHARP_GOLDEN");
    if (golden.empty() || !fs::is_directory(golden)) {
        MESSAGE("skipping: set SPLAT_SHARP_GOLDEN to the golden/ folder to run the parity test");
        return;
    }
    std::string error;
    fs::path gguf;
    if (fp16) {
        gguf = env_or_empty("SPLAT_SHARP_GGUF_F16");
        if (gguf.empty()) {
            MESSAGE("skipping: set SPLAT_SHARP_GGUF_F16 to the fp16 SHARP GGUF");
            return;
        }
    } else {
        gguf = ensure_fp32_gguf(&error);
        if (gguf.empty()) {
            MESSAGE("skipping fp32 case: "
                    << (error.empty() ? "set SPLAT_SHARP_GGUF to the fp32 SHARP GGUF (or "
                                        "SPLAT_SHARP_GGUF_F16 + SPLAT_SHARP_CHECKPOINT to build it)"
                                      : error));
            return;
        }
    }
    if (!fs::is_regular_file(gguf)) {
        MESSAGE("skipping: " << gguf << " is missing");
        return;
    }
    std::vector<GoldenPhoto> photos = golden_photos(golden);
    REQUIRE_MESSAGE(!photos.empty(), "no golden photos under " << golden);

    splat::SharpEncoderConfig config;
    splat::Result<std::unique_ptr<splat::SharpEncoder>> encoder =
        splat::SharpEncoder::create(gguf, prefer_vulkan, config);
    REQUIRE_MESSAGE(encoder.ok(), encoder.message());
    const bool on_vulkan =
        encoder.value()->backend().info().kind == splat::GgmlBackend::Kind::Vulkan;
    const Memory loaded = process_memory();
    MESSAGE("backend: " << encoder.value()->backend().describe() << ", weights " << mib(loaded.resident)
            << " RSS after load (" << mib(loaded.peak) << " peak)");

    for (const GoldenPhoto& photo : photos) {
        MESSAGE("photo " << photo.id << " (" << (fp16 ? "fp16" : "fp32") << " weights)");
        std::vector<float> input;
        int size = 0;
        if (!run_preprocess(photo, &input, &size, &error)) {
            FAIL("preprocess failed for " << photo.id << ": " << error);
            continue;
        }

        std::map<std::string, splat::SharpEncoderStage> stages;
        const Memory before = process_memory();
        const auto started = std::chrono::steady_clock::now();
        splat::Result<splat::SharpEncoderResult> result =
            encoder.value()->run(input.data(), size,
                                 [&stages](const splat::SharpEncoderStage& stage) {
                                     stages[stage.name] = stage;
                                 });
        const double wall =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        REQUIRE_MESSAGE(result.ok(), result.message());
        const Memory after = process_memory();
        const splat::SharpEncoderResult& encoder_result = result.value();
        MESSAGE("  stage seconds: pyramid " << encoder_result.pyramid_seconds << ", patch encoder "
                << encoder_result.patch_encoder_seconds << ", merge " << encoder_result.merge_seconds
                << ", image encoder " << encoder_result.image_encoder_seconds << ", upsample "
                << encoder_result.upsample_seconds << ", wall " << wall);
        MESSAGE("  submits " << encoder_result.submits << ", arena " << mib(encoder_result.arena_bytes)
                << ", RSS before " << mib(before.resident) << " after " << mib(after.resident)
                << " peak " << mib(after.peak));

        for (const StageExpectation& expectation : kEncoderStages) {
            const auto stage = stages.find(expectation.stage);
            if (stage == stages.end()) {
                FAIL("encoder did not report stage " << expectation.stage);
                continue;
            }
            const splat::parity::Stats stats = splat::parity::compare_npy(
                (photo.folder / expectation.file).string(), stage->second.shape,
                stage->second.values.data());
            print_stats(expectation.stage, stats, fp16 ? " [fp16]" : " [fp32]");
            REQUIRE_MESSAGE(stats.ok, stats.error);
            // The spec's fp16 gate is cosine >= 0.999 per tensor, and it is the
            // only gate on MoltenVK: the spec excludes it as a parity target
            // (docs/research/op-audit.md §1.5), so its fp32 error is reported
            // but not gated. Nothing is skipped silently.
            CHECK_MESSAGE(stats.cosine >= 0.999, expectation.stage << " cosine " << stats.cosine);
            if (!fp16 && !on_vulkan) {
                // The spec's fp32 gate, per element: 1e-3 maximum relative
                // error. Measured worst case over photo01..03 is 1.4e-4 rel and
                // 2.1e-6 abs, so the spec number holds with room to spare.
                CHECK_MESSAGE(stats.max_rel <= 1e-3,
                              expectation.stage << " max rel " << stats.max_rel);
                CHECK_MESSAGE(stats.max_abs <= 1e-5,
                              expectation.stage << " max abs " << stats.max_abs);
                CHECK_MESSAGE(stats.p99_rel <= 1e-3, expectation.stage << " p99 rel " << stats.p99_rel);
                CHECK_MESSAGE(stats.p999_rel <= 2e-3,
                              expectation.stage << " p99.9 rel " << stats.p999_rel);
                CHECK_MESSAGE(stats.cosine >= 0.99999,
                              expectation.stage << " fp32 cosine " << stats.cosine);
            }
        }
    }
}

}  // namespace

// The .npy reader (tools/parity/npy.h) must reject a malformed header with an
// error: a truncated shape tuple used to wrap around in substr() and throw
// std::out_of_range out of the parity test instead of failing the comparison.
TEST_CASE("sharp parity npy reader rejects malformed headers") {
    const fs::path directory = fs::temp_directory_path() / "splat_npy_test";
    std::error_code filesystem_error;
    fs::create_directories(directory, filesystem_error);
    REQUIRE_MESSAGE(!filesystem_error, filesystem_error.message());

    auto write_npy = [&](const std::string& name, const std::string& header,
                         const std::string& payload) {
        const fs::path path = directory / name;
        std::ofstream stream(path, std::ios::binary);
        REQUIRE(static_cast<bool>(stream));
        stream.write("\x93NUMPY", 6);
        stream.put('\x01');
        stream.put('\x00');
        const uint16_t length = static_cast<uint16_t>(header.size());
        stream.put(static_cast<char>(length & 0xff));
        stream.put(static_cast<char>(length >> 8));
        stream.write(header.data(), static_cast<std::streamsize>(header.size()));
        stream.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        return path;
    };
    auto header = [](const std::string& shape) {
        std::string text = "{'descr': '<f4', 'fortran_order': False, 'shape': " + shape + ", }";
        while (text.size() % 64 != 63) text.push_back(' ');
        return text + "\n";
    };

    std::vector<int64_t> shape;
    std::vector<float> values;
    std::string error;

    const fs::path unclosed = write_npy("unclosed.npy", header("(4, 4"), {});
    CHECK_FALSE(splat::parity::read_npy_f32(unclosed.string(), &shape, &values, &error));

    const fs::path bad_digit = write_npy("bad_digit.npy", header("(4, x4)"), {});
    error.clear();
    CHECK_FALSE(splat::parity::read_npy_f32(bad_digit.string(), &shape, &values, &error));
    CHECK_FALSE(error.empty());

    const fs::path overflow = write_npy("overflow.npy",
                                        header("(999999999999999999999999999999, 4)"), {});
    error.clear();
    CHECK_FALSE(splat::parity::read_npy_f32(overflow.string(), &shape, &values, &error));
    CHECK_FALSE(error.empty());

    // The same writer produces a readable file, so the rejections above are the
    // header checks and not a broken test fixture.
    const fs::path tiny = write_npy("tiny.npy", header("(2, 2)"), std::string(4 * sizeof(float), '\0'));
    error.clear();
    REQUIRE_MESSAGE(splat::parity::read_npy_f32(tiny.string(), &shape, &values, &error), error);
    CHECK(shape == std::vector<int64_t>{2, 2});
    CHECK(values.size() == 4);
}

TEST_CASE("sharp preprocess parity") {
    const fs::path golden = env_or_empty("SPLAT_SHARP_GOLDEN");
    if (golden.empty() || !fs::is_directory(golden)) {
        MESSAGE("skipping: set SPLAT_SHARP_GOLDEN to the golden/ folder");
        return;
    }
    const std::vector<GoldenPhoto> found = golden_photos(golden);
    // A golden folder without the photo ids must fail, not pass with zero
    // assertions (SPLAT_SHARP_PHOTOS may narrow the set, but it must not empty
    // it).
    REQUIRE_MESSAGE(!found.empty(), "no golden photo folders under " << golden);
    for (const GoldenPhoto& photo : found) {
        MESSAGE("photo " << photo.id);
        std::vector<float> input;
        int size = 0;
        std::string error;
        REQUIRE_MESSAGE(run_preprocess(photo, &input, &size, &error), error);
        CHECK(size == 1536);
        CHECK(input.size() == static_cast<size_t>(3) * size * size);
        double lowest = 0.0;
        double highest = 0.0;
        for (float value : input) {
            lowest = std::min(lowest, static_cast<double>(value));
            highest = std::max(highest, static_cast<double>(value));
        }
        CHECK(lowest >= -1.0);
        CHECK(highest <= 1.0);
    }
}

TEST_CASE("sharp encoder parity cpu fp32") { run_encoder_case(false, false); }

TEST_CASE("sharp encoder parity vulkan fp32") { run_encoder_case(true, false); }

TEST_CASE("sharp encoder parity cpu fp16") { run_encoder_case(false, true); }

TEST_CASE("sharp encoder parity vulkan fp16") { run_encoder_case(true, true); }
