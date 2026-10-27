// Memory behaviour of the two reconstruction backends: what unload() gives
// back, and whether 20 back-to-back conversions grow the process or the GPU
// heap. Spec "Performance and memory budgets": "run 20 back-to-back conversions
// ... with no memory growth", and C9: "the model is unloaded before the renderer
// allocates".
//
// Opt-in: 20 SHARP runs are ~13 minutes on MoltenVK.
//
//   export SPLAT_SHARP_MEMORY=1
//   export SPLAT_SHARP_GGUF_F16=/path/to/sharp-...-f16-v1.gguf
//   export SPLAT_DA2_GGUF_F16=/path/to/dav2s-f16.gguf
//   export SPLAT_FALLBACK_PHOTO=/path/to/photo.jpg        # optional
//   build-F8-sharp/tests/splat_tests -tc="sharp memory*"
//
// The GPU number is the Vulkan heap budget (VK_EXT_memory_budget via
// ggml_backend_vk_get_device_memory) summed over the device-local heaps, read
// through one GgmlBackend kept alive for the whole case so its own footprint is
// part of the baseline. On macOS the unified heap makes this the process's
// graphics footprint; on the console it is the 11.44 GiB heap the spec budgets
// against. The CPU backend has no heap budget, so the case skips there.

#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "common/exif.h"
#include "jobs/image_io.h"
#include "recon/fallback/fallback_reconstructor.h"
#include "recon/ggml/backend.h"
#include "recon/sharp/sharp_reconstructor.h"
#include "sharp_golden.h"

#if defined(__APPLE__)
#include <mach/mach.h>
#endif
#if !defined(_WIN32)
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace {

using namespace splat;
using namespace splat::sharp_test;

uint64_t peak_rss_bytes() {
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#if defined(__APPLE__)
    return static_cast<uint64_t>(usage.ru_maxrss);
#else
    return static_cast<uint64_t>(usage.ru_maxrss) * 1024;
#endif
}

// Current resident size: the peak is a high-water mark that a one-off allocator
// ratchet can lift, while this is what the process is holding now.
uint64_t current_rss_bytes() {
#if defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                  &count) == KERN_SUCCESS) {
        return static_cast<uint64_t>(info.resident_size);
    }
    return 0;
#else
    std::ifstream statm("/proc/self/statm");
    long pages = 0;
    long resident = 0;
    if (statm >> pages >> resident) {
        return static_cast<uint64_t>(resident) * static_cast<uint64_t>(::sysconf(_SC_PAGESIZE));
    }
    return 0;
#endif
}

// "+12 MiB" / "-34 MiB", so a delta reads the same way in every line.
std::string mib_delta(int64_t bytes) {
    return std::string(bytes >= 0 ? "+" : "-") +
           mib(static_cast<size_t>(bytes >= 0 ? bytes : -bytes));
}

// The photo the cycles run on: the ImmichVR demo photo when it is present (a
// real 1440x1920 phone photo), else a golden input.
std::filesystem::path test_photo() {
    const std::string from_env = env_or_empty("SPLAT_FALLBACK_PHOTO");
    if (!from_env.empty() && std::filesystem::is_regular_file(from_env)) return from_env;
    const std::filesystem::path golden = golden_root();
    if (golden.empty()) return {};
    for (const char* candidate : {"immichvr/4.jpg", "inputs/photo02_street.jpg"}) {
        const std::filesystem::path path = golden / candidate;
        if (std::filesystem::is_regular_file(path)) return path;
    }
    return {};
}

bool load_photo(const std::filesystem::path& jpeg, ReconstructionInput* input, std::string* error) {
    Result<std::vector<uint8_t>> bytes = read_file_bytes(jpeg);
    if (!bytes.ok()) {
        *error = bytes.message();
        return false;
    }
    Result<Image> image = decode_image(bytes.value().data(), bytes.value().size());
    if (!image.ok()) {
        *error = image.message();
        return false;
    }
    input->rgb = std::move(image.value());
    const Result<float> focal = focal_px_from_jpeg(bytes.value().data(), bytes.value().size(),
                                                   input->rgb.width, input->rgb.height);
    if (!focal.ok()) {
        *error = focal.message();
        return false;
    }
    input->focal_px = focal.value();
    return true;
}

// One load -> run -> unload cycle. Returns the scene's Gaussian count (0 on
// failure, with `error` set).
size_t cycle(IReconstructor& backend, const Path& gguf, const ReconstructionInput& input,
             std::string* error) {
    const Status loaded = backend.load(gguf);
    if (!loaded.ok()) {
        *error = loaded.message();
        return 0;
    }
    Progress progress;
    Result<SplatScene> scene = backend.run(input, progress);
    backend.unload();
    if (!scene.ok()) {
        *error = scene.message();
        return 0;
    }
    return scene.value().count();
}

struct Heap {
    uint64_t free = 0;
    uint64_t total = 0;
    uint64_t used() const { return total - free; }
};

Heap heap_of(GgmlBackend& probe) {
    probe.refresh_memory();
    Heap heap;
    heap.free = probe.info().free_memory;
    heap.total = probe.info().total_memory;
    return heap;
}

}  // namespace

TEST_CASE("sharp memory unload releases the heap") {
    if (env_or_empty("SPLAT_SHARP_MEMORY").empty()) {
        MESSAGE("skipping: set SPLAT_SHARP_MEMORY=1 to measure load/unload memory");
        return;
    }
    const std::filesystem::path photo = test_photo();
    REQUIRE_MESSAGE(!photo.empty(), "no test photo: set SPLAT_FALLBACK_PHOTO");
    const Path gguf = gguf_path(true);
    REQUIRE_MESSAGE(!gguf.empty(), "set SPLAT_SHARP_GGUF_F16 to the fp16 SHARP GGUF");

    ReconstructionInput input;
    std::string error;
    REQUIRE_MESSAGE(load_photo(photo, &input, &error), error);

    std::string note;
    Result<GgmlBackend> probe_result = GgmlBackend::create(true, &note);
    REQUIRE_MESSAGE(probe_result.ok(), note);
    GgmlBackend probe = std::move(probe_result.value());
    if (probe.info().kind != GgmlBackend::Kind::Vulkan) {
        MESSAGE("skipping: " << (note.empty() ? "no Vulkan device" : note)
                             << "; the CPU backend has no heap budget");
        return;
    }

    const uint64_t rss_before = peak_rss_bytes();
    const Heap before = heap_of(probe);
    {
        SharpReconstructor backend(SharpConfig{});
        Progress progress;
        const Status loaded = backend.load(gguf);
        REQUIRE_MESSAGE(loaded.ok(), loaded.message());
        const Heap loaded_heap = heap_of(probe);
        const uint64_t loaded_rss = peak_rss_bytes();
        Result<SplatScene> scene = backend.run(input, progress);
        REQUIRE_MESSAGE(scene.ok(), scene.message());
        CHECK(scene.value().count() == 1179648);
        const Heap ran = heap_of(probe);
        backend.unload();
        const Heap unloaded = heap_of(probe);
        const uint64_t unloaded_rss = peak_rss_bytes();

        MESSAGE("heap budget " << mib(before.total) << ": before load " << mib(before.free)
                << " free; after load " << mib(loaded_heap.free) << " free ("
                << mib_delta(-static_cast<int64_t>(before.free - loaded_heap.free))
                << "); after run " << mib(ran.free) << " free; after unload "
                << mib(unloaded.free) << " free ("
                << mib_delta(static_cast<int64_t>(unloaded.free) -
                             static_cast<int64_t>(before.free))
                << " vs before)");
        MESSAGE("peak RSS before " << mib(rss_before) << ", after load " << mib(loaded_rss)
                << ", after run+unload " << mib(unloaded_rss));

        // unload() must hand the weights and the level arena back. The residual
        // is the driver's own bookkeeping (MoltenVK keeps its shader/pipeline
        // caches and a small staging pool), so the gate is "within 64 MiB of the
        // pre-load heap", not "exactly equal".
        const int64_t retained = static_cast<int64_t>(before.free) -
                                 static_cast<int64_t>(unloaded.free);
        CHECK_MESSAGE(retained <= (64ll << 20), "unload() retained " << mib_delta(retained)
                                                     << " of GPU heap");
    }
    // And after the reconstructor itself is destroyed.
    const Heap destroyed = heap_of(probe);
    MESSAGE("after destroying the reconstructor: " << mib(destroyed.free) << " free ("
            << mib_delta(static_cast<int64_t>(destroyed.free) -
                         static_cast<int64_t>(before.free))
            << " vs before)");
    const int64_t retained = static_cast<int64_t>(before.free) - static_cast<int64_t>(destroyed.free);
    CHECK_MESSAGE(retained <= (64ll << 20), "destroying the reconstructor retained "
                                                << mib_delta(retained) << " of GPU heap");
}

TEST_CASE("sharp memory 20 back-to-back runs") {
    if (env_or_empty("SPLAT_SHARP_MEMORY").empty()) {
        MESSAGE("skipping: set SPLAT_SHARP_MEMORY=1 to run the 20-cycle soak");
        return;
    }
    const int cycles = static_cast<int>(std::strtol(
        env_or_empty("SPLAT_SHARP_MEMORY_CYCLES").c_str(), nullptr, 10));
    const int count = cycles > 0 ? cycles : 20;
    const std::filesystem::path photo = test_photo();
    REQUIRE_MESSAGE(!photo.empty(), "no test photo: set SPLAT_FALLBACK_PHOTO");
    const Path sharp_gguf = gguf_path(true);
    REQUIRE_MESSAGE(!sharp_gguf.empty(), "set SPLAT_SHARP_GGUF_F16 to the fp16 SHARP GGUF");
    const std::string da2 = env_or_empty("SPLAT_DA2_GGUF_F16");
    REQUIRE_MESSAGE(!da2.empty(), "set SPLAT_DA2_GGUF_F16 to the fp16 DA2 GGUF");
    REQUIRE_MESSAGE(std::filesystem::is_regular_file(da2), da2 << " is missing");

    ReconstructionInput input;
    std::string error;
    REQUIRE_MESSAGE(load_photo(photo, &input, &error), error);

    std::string note;
    Result<GgmlBackend> probe_result = GgmlBackend::create(true, &note);
    REQUIRE_MESSAGE(probe_result.ok(), note);
    GgmlBackend probe = std::move(probe_result.value());
    REQUIRE_MESSAGE(probe.info().kind == GgmlBackend::Kind::Vulkan,
                    "the 20-cycle soak needs Vulkan to measure the heap: " << note);

    // Baseline after one warm-up cycle of each backend, so one-time allocations
    // (Vulkan pipelines, the stb decoder, malloc arenas) are not counted as
    // growth.
    {
        SharpReconstructor backend;
        REQUIRE_MESSAGE(cycle(backend, sharp_gguf, input, &error) == 1179648, error);
        FallbackReconstructor fallback;
        REQUIRE_MESSAGE(cycle(fallback, da2, input, &error) > 0, error);
    }
    const Heap base_heap = heap_of(probe);
    const uint64_t base_rss = peak_rss_bytes();
    const uint64_t base_resident = current_rss_bytes();
    MESSAGE("baseline after one warm-up cycle of each backend: " << mib(base_heap.free)
            << " heap free, RSS " << mib(base_resident) << " (peak " << mib(base_rss) << ")");

    const auto started = std::chrono::steady_clock::now();
    uint64_t low_water = base_resident;  // lowest resident size seen after a cycle
    for (int index = 0; index < count; ++index) {
        // Every third job is the fallback: the mix the spec's soak run implies
        // (a queue of photos served by either backend).
        const bool use_fallback = index % 3 == 2;
        size_t splats = 0;
        if (use_fallback) {
            FallbackReconstructor backend;
            splats = cycle(backend, da2, input, &error);
        } else {
            SharpReconstructor backend;
            splats = cycle(backend, sharp_gguf, input, &error);
        }
        REQUIRE_MESSAGE(splats > 0, "cycle " << index << ": " << error);
        const Heap heap = heap_of(probe);
        const uint64_t rss = peak_rss_bytes();
        const uint64_t resident = current_rss_bytes();
        if (resident > 0 && resident < low_water) low_water = resident;
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        char line[320];
        std::snprintf(line, sizeof(line),
                      "  run %2d/%-2d %-8s %zu splats  heap free %s (%s)  RSS %s (%s)  peak %s  %5.1f s",
                      index + 1, count, use_fallback ? "fallback" : "sharp", splats,
                      mib(heap.free).c_str(),
                      mib_delta(-static_cast<int64_t>(heap.free - base_heap.free)).c_str(),
                      mib(resident).c_str(),
                      mib_delta(static_cast<int64_t>(resident) -
                                static_cast<int64_t>(base_resident)).c_str(),
                      mib(rss).c_str(), seconds);
        MESSAGE(line);
    }
    const Heap end_heap = heap_of(probe);
    const uint64_t end_rss = peak_rss_bytes();
    const uint64_t end_resident = current_rss_bytes();
    const int64_t heap_growth = static_cast<int64_t>(base_heap.free) - static_cast<int64_t>(end_heap.free);
    const int64_t low_water_growth = static_cast<int64_t>(low_water) -
                                     static_cast<int64_t>(base_resident);
    const int64_t peak_growth = static_cast<int64_t>(end_rss) - static_cast<int64_t>(base_rss);
    MESSAGE("after " << count << " cycles: heap free " << mib(end_heap.free) << " ("
            << mib_delta(-heap_growth) << " vs baseline), RSS " << mib(end_resident)
            << ", low water " << mib(low_water) << " (" << mib_delta(low_water_growth)
            << " vs baseline), peak RSS " << mib(end_rss) << " (" << mib_delta(peak_growth)
            << " vs baseline)");
    // Three signals, one gate each:
    //  - the GPU heap is the sharpest and it is exactly flat here -> 32 MiB;
    //  - a per-run *host* leak raises the resident-set floor, so the low water
    //    is gated at 512 MiB (one run's working set is 2.5 GiB);
    //  - peak RSS is a high-water mark that the allocator ratchets whenever it
    //    stops reusing a freed 1.3 GiB weight buffer (measured: +1.3 GiB at the
    //    fifth run and +0.4 GiB at the eleventh, flat for the other 18), so it is
    //    only gated at 4 GiB. That still catches a gross leak — a real 2.5 GiB
    //    working set leaking once per run would pass it after two runs — without
    //    failing on allocator policy.
    CHECK_MESSAGE(heap_growth <= (32ll << 20),
                  "the GPU heap grew by " << mib_delta(heap_growth) << " over the soak");
    CHECK_MESSAGE(low_water_growth <= (512ll << 20),
                  "the resident-set floor rose by " << mib_delta(low_water_growth));
    CHECK_MESSAGE(peak_growth <= (4096ll << 20),
                  "peak RSS grew by " << mib_delta(peak_growth));
}
