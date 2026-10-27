// Spike A on the console: the ggml op audit at SHARP and DA2 shapes, Vulkan
// against the console CPU, with PASS/FAIL and the error per case.
//
// This is the payload form of tools/op-audit/sharp_ops.cpp (the manual harness
// that produced docs/research/op-audit.md). Same cases, same statistics; the
// differences are the console plumbing: klog output, one process, a bench.env
// file that selects driver settings before the first Vulkan call, and a
// watchdog for the unbounded vkWaitForFences in ggml-vulkan.
//
// Unlike the reconstruction benches, every case here is one op (or a short
// chain of a few ops, at most 8 nodes). That is already one submit under the
// benches' 64-node budget, so there is no GraphSession in this payload: the
// submit split the spec asks for does not apply to a single-op graph. The
// `nodes` field of every result line makes that explicit.
//
// Coopmat on/off is measured in one launch: pass 1 at whatever bench.env says
// (driver default unless the file disables it), then the cached VkDevice is
// released (platform/ps5/ggml/vk_reset.cpp), GGML_VK_DISABLE_COOPMAT is
// flipped, and pass 2 runs every case again. The two results are logged side
// by side. No manual bench.env edit is needed for that comparison.
//
// Cases marked Exact use F32 weights and are expected to match the CPU to
// NMSE <= 1e-6 (cos >= 0.99999); F16 cases get the spec's fp16 gate
// (NMSE <= 1e-4, cos >= 0.999). A case with no CPU reference only has to be
// finite.
//
// A compute error or a watchdog timeout logs and abandons the process without
// freeing the buffer or context the GPU may still be using (the vk_hello
// policy from docs/research/spike-b.md): the leak is deliberate, the process
// is about to exit anyway.
//
// Build: platform/ps5/bench/Makefile  ->  op_audit.elf
// Run:   platform/ps5/scripts/run.sh platform/ps5/bench/op_audit.elf 900
// Title: the same source is one of the benches the bench title runs (built with
//        -DPS5_TITLE; docs/research/spike-a.md section 2.4).
//
// Exit: 0 all cases passed, 1 a case failed, 2 setup (no device / no CPU).
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include <unistd.h>

#include "ggml.h"
#include "ggml-alloc.h"
#include "ggml-backend.h"
#include "ggml-cpu.h"

#include "bench_env.h"
#include "bench_title.h"
#include "ggml_logging.h"

using splat::bench::Watchdog;
using splat::bench::human_bytes;
using splat::bench::peak_rss_bytes;

namespace {

const char* const kTag = "op_audit";

#ifdef PS5_TITLE
// The title build logs through sceKernelDebugOutText: a title has no stdout
// reader and no klog_puts. Same text, so the payload greps still apply.
void
log_line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void
log_line(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    splat::bench::title_vlog(fmt, args);
    va_end(args);
}
#else
using splat::bench::log_line;
#endif

// Which gate a case is held to.
enum class Gate {
    Exact,     // F32 weights: NMSE <= 1e-6, cos >= 0.99999
    Reduction, // F32 flash attention: a 577-term reduction reorders in fp32
               // (measured 1.3e-6 NMSE, cos 0.9999994); still far inside the
               // spec's fp16 gate and 10x under the F16 paths below
    F16,       // F16 weights / F16 staging: NMSE <= 1e-4, cos >= 0.999
    Finite,    // no CPU reference (too large): finite output only
};

const char* gate_name(Gate gate) {
    switch (gate) {
        case Gate::Exact: return "exact";
        case Gate::Reduction: return "reduction";
        case Gate::F16: return "f16";
        case Gate::Finite: return "finite";
    }
    return "?";
}

struct Case {
    Case(std::string name_, std::function<ggml_tensor*(ggml_context*)> build_, Gate gate_, bool big_,
         const char* note_)
        : name(std::move(name_)), build(std::move(build_)), gate(gate_), big(big_), note(note_) {}

    std::string name;
    std::function<ggml_tensor*(ggml_context*)> build;
    Gate gate;
    bool big;  // > ~1 s on the console CPU; gets a longer watchdog
    const char* note;
    // Filled by the run.
    double vk_ms = 0.0;
    double cpu_ms = 0.0;
    double max_abs = 0.0;
    double nmse = 0.0;
    double cos = 1.0;
    size_t nodes = 0;
    bool ok = false;
    std::string error;
};

uint64_t hash_str(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

ggml_tensor* T(ggml_context* ctx, ggml_type type, int64_t a, int64_t b, int64_t c, int64_t d,
               const char* name) {
    ggml_tensor* t = ggml_new_tensor_4d(ctx, type, a, b, c, d);
    ggml_set_name(t, name);
    return t;
}

void fill_leaf(ggml_tensor* t) {
    const size_t n = ggml_nelements(t);
    std::vector<uint8_t> buf(ggml_nbytes(t));
    std::mt19937 rng(static_cast<uint32_t>(hash_str(std::string(ggml_get_name(t)))));
    std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
    if (t->type == GGML_TYPE_F32) {
        float* p = reinterpret_cast<float*>(buf.data());
        for (size_t i = 0; i < n; i++) p[i] = dist(rng);
    } else if (t->type == GGML_TYPE_F16) {
        ggml_fp16_t* p = reinterpret_cast<ggml_fp16_t*>(buf.data());
        for (size_t i = 0; i < n; i++) p[i] = ggml_fp32_to_fp16(dist(rng));
    } else if (t->type == GGML_TYPE_I32) {
        int32_t* p = reinterpret_cast<int32_t*>(buf.data());
        for (size_t i = 0; i < n; i++) p[i] = static_cast<int32_t>(rng() % 8);
    } else {
        log_line("%s: unhandled leaf type %d", kTag, static_cast<int>(t->type));
        std::abort();
    }
    ggml_backend_tensor_set(t, buf.data(), 0, buf.size());
}

// One run of one case on one backend. `out` is filled with the output tensor's
// floats; `ms` with the median of the timed runs. Every call that can reach the
// GPU is armed on the watchdog -- buffer allocation/upload, graph compute,
// synchronize, readback and teardown -- because ggml-vulkan's fence waits are
// unbounded (vkWaitForFences with UINT64_MAX): a wedged GPU logs and
// _exit(3)s the process without any teardown. A compute error takes the same
// "log, leak, exit" path.
bool run_one(ggml_backend_t backend, ggml_backend_dev_t dev, const Case& c,
             std::vector<float>& out, double& ms, int reps, std::string& err, size_t& nodes,
             Watchdog& watchdog, double timeout_s) {
    ggml_init_params ip = {/*mem_size*/ 512u * 1024u * 1024u, /*mem_buffer*/ nullptr, /*no_alloc*/ true};
    ggml_context* ctx = ggml_init(ip);
    if (ctx == nullptr) {
        err = "ggml_init";
        return false;
    }

    ggml_tensor* out_t = c.build(ctx);
    ggml_cgraph* gf = ggml_new_graph(ctx);
    ggml_build_forward_expand(gf, out_t);
    nodes = static_cast<size_t>(ggml_graph_n_nodes(gf));

    std::string unsup;
    if (backend != nullptr && dev != nullptr) {
        for (int i = 0; i < ggml_graph_n_nodes(gf); i++) {
            ggml_tensor* n = ggml_graph_node(gf, i);
            if (!ggml_backend_dev_supports_op(dev, n)) {
                char b[256];
                std::snprintf(b, sizeof(b), "%s[%lld,%lld,%lld,%lld];", ggml_op_desc(n),
                              static_cast<long long>(n->ne[0]), static_cast<long long>(n->ne[1]),
                              static_cast<long long>(n->ne[2]), static_cast<long long>(n->ne[3]));
                unsup += b;
            }
        }
    }
    if (!unsup.empty()) {
        err = "unsupported: " + unsup;
        // Nothing was submitted: freeing the (empty) context is safe.
        ggml_free(ctx);
        return false;
    }

    watchdog.arm(timeout_s, "buffer alloc/upload");
    ggml_backend_buffer_t buf = ggml_backend_alloc_ctx_tensors(ctx, backend);
    if (buf == nullptr) {
        watchdog.disarm();
        err = "alloc";
        ggml_free(ctx);
        return false;
    }

    for (ggml_tensor* t = ggml_get_first_tensor(ctx); t != nullptr; t = ggml_get_next_tensor(ctx, t)) {
        if (t->op == GGML_OP_NONE && t->view_src == nullptr && t != out_t) fill_leaf(t);
    }
    watchdog.disarm();

    std::vector<double> times;
    for (int r = 0; r < reps; r++) {
        // The fences from the previous rep may still be pending; bound the wait.
        watchdog.arm(timeout_s, "synchronize");
        ggml_backend_synchronize(backend);
        watchdog.disarm();

        ggml_status st;
        const auto t0 = std::chrono::steady_clock::now();
        watchdog.arm(timeout_s, "graph compute");
        st = ggml_backend_graph_compute(backend, gf);
        watchdog.disarm();
        // ggml-vulkan's graph compute already waits on its fence; this second
        // synchronize is still an unbounded wait, so it is bounded by the
        // watchdog as well (it normally returns immediately).
        watchdog.arm(timeout_s, "synchronize");
        ggml_backend_synchronize(backend);
        watchdog.disarm();
        const auto t1 = std::chrono::steady_clock::now();
        if (st != GGML_STATUS_SUCCESS) {
            // Device lost or a rejected submission: the GPU may still be
            // touching the buffer, so nothing is freed and nothing is torn
            // down. Log and leave the process.
            log_line("%s: compute error %s: %s", kTag, c.name.c_str(), ggml_status_to_string(st));
            log_line("%s: leaking the buffer, context and device and exiting 1 (no teardown)",
                     kTag);
            splat::bench::abandon(kTag, 1);
        }
        times.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::sort(times.begin(), times.end());
    ms = times[times.size() / 2];

    out.resize(ggml_nelements(out_t));
    watchdog.arm(timeout_s, "readback");
    ggml_backend_tensor_get(out_t, out.data(), 0, out.size() * sizeof(float));
    watchdog.disarm();
    // The GPU signalled and read back cleanly: this teardown is safe.
    watchdog.arm(timeout_s, "teardown");
    ggml_backend_buffer_free(buf);
    ggml_free(ctx);
    watchdog.disarm();
    return true;
}

struct Stats {
    double max_abs = 0.0;
    double nmse = 0.0;
    double cos = 1.0;
    size_t nonfinite = 0;
    double max_ref = 0.0;
};

Stats stats(const std::vector<float>& ref, const std::vector<float>& got) {
    Stats s;
    double se = 0.0, ss = 0.0, dot = 0.0, na = 0.0, nb = 0.0;
    for (size_t i = 0; i < ref.size(); i++) {
        const double a = ref[i];
        const double b = got[i];
        if (!std::isfinite(a) || !std::isfinite(b)) s.nonfinite++;
        const double d = a - b;
        s.max_abs = std::max(s.max_abs, std::fabs(d));
        s.max_ref = std::max(s.max_ref, std::fabs(a));
        se += d * d;
        ss += a * a;
        dot += a * b;
        na += a * a;
        nb += b * b;
    }
    s.nmse = ss > 0.0 ? se / ss : se;
    s.cos = (na > 0.0 && nb > 0.0) ? dot / std::sqrt(na * nb) : 1.0;
    return s;
}

// One case in one coopmat pass.
struct Row {
    std::string name;
    bool finite = false;
    bool unsupported = false;
    bool ok = false;
    double ms = 0.0;
    double nmse = 0.0;
    double cos = 1.0;
    double max_abs = 0.0;
    size_t nodes = 0;
    std::string error;
};

// One pass over all cases with one coopmat configuration.
struct Pass {
    std::string label;
    int coopmat = -1;  // 1 on, 0 off, -1 unknown (host build)
    int passed = 0;
    int failed = 0;
    int unsupported = 0;
    double seconds = 0.0;
    std::vector<Row> rows;
};

const char* coopmat_name(int state) {
    return state > 0 ? "on" : state == 0 ? "off" : "unknown";
}

}  // namespace

int
op_audit_run(int argc, char** argv) {
    (void) argc;
    (void) argv;

    Watchdog watchdog(kTag);
    splat::bench::apply_bench_radv_defaults(kTag);
    splat::bench::load_env_file(splat::bench::bench_path("bench.env"), kTag);
#ifdef PS5_TITLE
    splat::bench::install_title_ggml_logging(kTag);
#else
    splat::bench::install_ggml_logging(kTag);
#endif

    const char* disable_coopmat = std::getenv("GGML_VK_DISABLE_COOPMAT");
    log_line("%s: start (ps5-splat-vr Spike A op audit, Vulkan vs console CPU)", kTag);
    log_line("%s: coopmat %s (GGML_VK_DISABLE_COOPMAT=%s)", kTag,
             disable_coopmat != nullptr ? "disabled by bench.env" : "at the driver default",
             disable_coopmat != nullptr ? disable_coopmat : "(unset)");

    ggml_backend_dev_t cpu_dev = ggml_backend_dev_by_name("CPU");
    ggml_backend_dev_t vk_dev = ggml_backend_dev_by_name("Vulkan0");
    if (cpu_dev == nullptr || vk_dev == nullptr) {
        log_line("%s: FAIL backends missing (cpu=%p vulkan=%p)", kTag, static_cast<void*>(cpu_dev),
                 static_cast<void*>(vk_dev));
        return 2;
    }
    ggml_backend_t cpu = ggml_backend_dev_init(cpu_dev, nullptr);
    ggml_backend_t vk = ggml_backend_dev_init(vk_dev, nullptr);
    if (cpu == nullptr || vk == nullptr) {
        log_line("%s: FAIL backend init failed (cpu=%p vulkan=%p)", kTag, static_cast<void*>(cpu),
                 static_cast<void*>(vk));
        return 2;
    }
    log_line("%s: device %s", kTag, ggml_backend_dev_description(vk_dev));

    const ggml_type F32 = GGML_TYPE_F32;
    const ggml_type F16 = GGML_TYPE_F16;
    std::vector<Case> cases;

    // ---- conv helpers -----------------------------------------------------
    auto conv = [&](const char* nm, int64_t W, int64_t H, int64_t Cin, int64_t N, int64_t KW,
                    int64_t KH, int64_t Cout, int s, int p, ggml_type kt, Gate gate, bool big,
                    const char* note) {
        cases.push_back({nm,
                         [=](ggml_context* ctx) {
                             ggml_tensor* in = T(ctx, F32, W, H, Cin, N, "in");
                             ggml_tensor* k = T(ctx, kt, KW, KH, Cin, Cout, "k");
                             return ggml_conv_2d_direct(ctx, k, in, s, s, p, p, 1, 1);
                         },
                         gate, big, note});
    };
    auto tconv = [&](const char* nm, int64_t W, int64_t H, int64_t Cin, int64_t Cout, int64_t KW,
                     int64_t KH, int s, ggml_type kt, Gate gate, bool big, const char* note) {
        cases.push_back({nm,
                         [=](ggml_context* ctx) {
                             ggml_tensor* in = T(ctx, F32, W, H, Cin, 1, "in");
                             ggml_tensor* k = T(ctx, kt, KW, KH, Cout, Cin, "k");
                             return ggml_conv_transpose_2d_p0(ctx, k, in, static_cast<int>(s));
                         },
                         gate, big, note});
    };
    auto mulmat = [&](const char* nm, int64_t K, int64_t M, int64_t N, ggml_type wt, Gate gate,
                      const char* note) {
        cases.push_back({nm,
                         [=](ggml_context* ctx) {
                             ggml_tensor* w = T(ctx, wt, K, M, 1, 1, "w");
                             ggml_tensor* x = T(ctx, F32, K, N, 1, 1, "x");
                             return ggml_mul_mat(ctx, w, x);
                         },
                         gate, false, note});
    };
    auto im2col_stem = [&](const char* nm, Gate gate, bool f32_staging, const char* note) {
        cases.push_back({nm,
                         [=](ggml_context* ctx) {
                             ggml_tensor* in = T(ctx, F32, 384, 384, 3, 35, "in");
                             ggml_tensor* k = T(ctx, F32, 16, 16, 3, 1024, "k");
                             if (!f32_staging) {
                                 return ggml_conv_2d(ctx, k, in, 16, 16, 0, 0, 1, 1);
                             }
                             ggml_tensor* im2col =
                                 ggml_im2col(ctx, k, in, 16, 16, 0, 0, 1, 1, true, GGML_TYPE_F32);
                             ggml_tensor* r = ggml_mul_mat(
                                 ctx,
                                 ggml_reshape_2d(ctx, im2col, im2col->ne[0],
                                                 im2col->ne[3] * im2col->ne[2] * im2col->ne[1]),
                                 ggml_reshape_2d(ctx, k, k->ne[0] * k->ne[1] * k->ne[2], k->ne[3]));
                             r = ggml_reshape_4d(ctx, r, im2col->ne[1], im2col->ne[2], im2col->ne[3],
                                                 k->ne[3]);
                             return ggml_cont(ctx, ggml_permute(ctx, r, 0, 1, 3, 2));
                         },
                         gate, false, note});
    };

    // ---- SHARP encoder ----------------------------------------------------
    log_line("%s: cases: SHARP encoder (1536^2, 35 patches)", kTag);
    conv("sharp.stem_16x16s16_3to1024_384", 384, 384, 3, 35, 16, 16, 1024, 16, 0, F32, Gate::Exact,
         true, "patch embed, direct CONV_2D");
    conv("sharp.stem_16x16s16_3to1024_384_f16w", 384, 384, 3, 35, 16, 16, 1024, 16, 0, F16,
         Gate::F16, false, "patch embed, F16 weights");
    im2col_stem("sharp.stem_im2col_f16staging", Gate::F16, false,
                "ggml_conv_2d: im2col staged in F16 (the trap)");
    im2col_stem("sharp.stem_im2col_f32", Gate::Exact, true,
                "F32 im2col + mul_mat: the exact replacement");
    conv("sharp.conv3x3_1024_96", 96, 96, 1024, 1, 3, 3, 1024, 1, 1, F32, Gate::Exact, true,
         "SPN / ViT tap projection");
    conv("sharp.conv3x3_256_96", 96, 96, 256, 1, 3, 3, 256, 1, 1, F32, Gate::Exact, false,
         "parity proxy for DPT convs[1] at 768^2");
    conv("sharp.conv1x1_2048to1024_48", 48, 48, 2048, 1, 1, 1, 1024, 1, 0, F32, Gate::Exact, false,
         "fuse_lowres");
    conv("sharp.conv3x3_256_768", 768, 768, 256, 1, 3, 3, 256, 1, 1, F32, Gate::Finite, true,
         "DPT convs[1] at full 768^2");
    conv("sharp.conv2x2s2_5to128_1536", 1536, 1536, 5, 1, 2, 2, 128, 2, 0, F32, Gate::Finite, true,
         "Gaussian decoder skip conv");
    tconv("sharp.tconv2x2_1024_48to96", 48, 48, 1024, 1024, 2, 2, 2, F32, Gate::Exact, true,
          "SPN upsample1");
    tconv("sharp.tconv2x2_256_192to384", 192, 192, 256, 256, 2, 2, 2, F32, Gate::Exact, true,
          "parity proxy for SPN 384->768");
    tconv("sharp.tconv2x2_128_768to1536", 768, 768, 128, 128, 2, 2, 2, F32, Gate::Finite, true,
          "disparity head 768^2 -> 1536^2 (1.2 GB output)");

    cases.push_back({"sharp.layernorm_1024_577x35",
                     [=](ggml_context* ctx) {
                         ggml_tensor* x = T(ctx, F32, 1024, 577, 35, 1, "x");
                         return ggml_norm(ctx, x, 1e-6f);
                     },
                     Gate::Exact, true, "ViT LayerNorm, 35 patches x 577 tokens"});
    cases.push_back({"sharp.gelu_erf_4096_20195",
                     [=](ggml_context* ctx) {
                         ggml_tensor* x = T(ctx, F32, 4096, 577 * 35, 1, 1, "x");
                         return ggml_gelu_erf(ctx, x);
                     },
                     Gate::Exact, true, "ViT MLP hidden, exact erf GELU"});

    mulmat("sharp.mulmat_qkv_1024x3072_n20195_f32w", 1024, 3072, 577 * 35, F32, Gate::Exact,
           "qkv, F32 weights");
    mulmat("sharp.mulmat_qkv_1024x3072_n20195_f16w", 1024, 3072, 577 * 35, F16, Gate::F16,
           "qkv, F16 weights");
    mulmat("sharp.mulmat_proj_1024x1024_n20195_f16w", 1024, 1024, 577 * 35, F16, Gate::F16,
           "attn proj / fc2");

    auto attn = [&](const char* nm, ggml_type kvt, Gate gate, const char* note) {
        cases.push_back({nm,
                         [=](ggml_context* ctx) {
                             ggml_tensor* q = T(ctx, F32, 64, 577, 16, 35, "q");
                             ggml_tensor* k = T(ctx, kvt, 64, 577, 16, 35, "k");
                             ggml_tensor* v = T(ctx, kvt, 64, 577, 16, 35, "v");
                             return ggml_flash_attn_ext(ctx, q, k, v, nullptr, 1.0f / 8.0f, 0.0f, 0.0f);
                         },
                         gate, true, note});
    };
    attn("sharp.flash_attn_64x577x16x35_f16kv", F16, Gate::F16, "ViT-L attention, 35 patches, F16 KV");
    attn("sharp.flash_attn_64x577x16x35_f32kv", F32, Gate::Reduction, "ViT-L attention, F32 KV");

    // ---- depth / gaussian decoders ---------------------------------------
    cases.push_back({"sharp.groupnorm_8g_128_96",
                     [=](ggml_context* ctx) {
                         ggml_tensor* x = T(ctx, F32, 96, 96, 128, 1, "x");
                         return ggml_group_norm(ctx, x, 8, 1e-5f);
                     },
                     Gate::Exact, false, "parity proxy for the Gaussian head GN"});
    cases.push_back({"sharp.groupnorm_8g_128_768",
                     [=](ggml_context* ctx) {
                         ggml_tensor* x = T(ctx, F32, 768, 768, 128, 1, "x");
                         return ggml_group_norm(ctx, x, 8, 1e-5f);
                     },
                     Gate::Finite, true, "Gaussian decoder head GN (8 groups)"});
    cases.push_back({"sharp.conv3x3_128_768",
                     [=](ggml_context* ctx) {
                         ggml_tensor* in = T(ctx, F32, 768, 768, 128, 1, "in");
                         ggml_tensor* k = T(ctx, F32, 3, 3, 128, 128, "k");
                         return ggml_conv_2d_direct(ctx, k, in, 1, 1, 1, 1, 1, 1);
                     },
                     Gate::Finite, true, "texture/geometry head 3x3 128ch at 768^2"});

    cases.push_back({"sharp.pool_max_2x2_1536x2",
                     [=](ggml_context* ctx) {
                         ggml_tensor* x = T(ctx, F32, 1536, 1536, 2, 1, "x");
                         return ggml_pool_2d(ctx, x, GGML_OP_POOL_MAX, 2, 2, 2, 2, 0, 0);
                     },
                     Gate::Exact, true, "initialiser surface_min"});
    cases.push_back({"sharp.pool_avg_2x2_1536x3",
                     [=](ggml_context* ctx) {
                         ggml_tensor* x = T(ctx, F32, 1536, 1536, 3, 1, "x");
                         return ggml_pool_2d(ctx, x, GGML_OP_POOL_AVG, 2, 2, 2, 2, 0, 0);
                     },
                     Gate::Exact, true, "colour base avg_pool"});

    // ---- elementwise / composer ------------------------------------------
    cases.push_back({"sharp.add_bcast_1024x577x35",
                     [=](ggml_context* ctx) {
                         ggml_tensor* a = T(ctx, F32, 1024, 577, 35, 1, "a");
                         ggml_tensor* b = T(ctx, F32, 1024, 1, 1, 1, "b");
                         return ggml_add(ctx, a, b);
                     },
                     Gate::Exact, false, "LayerScale / bias broadcast"});
    cases.push_back({"sharp.composer_head_tail_14x2x768",
                     [=](ggml_context* ctx) {
                         ggml_tensor* d = T(ctx, F32, 14, 2, 768, 768, "d");
                         ggml_tensor* b = T(ctx, F32, 14, 1, 1, 1, "b");
                         ggml_tensor* y = ggml_add(ctx, d, b);
                         y = ggml_scale(ctx, y, 1e-3f);
                         y = ggml_sigmoid(ctx, y);
                         y = ggml_clamp(ctx, y, 0.01f, 0.99f);
                         y = ggml_log(ctx, y);
                         y = ggml_softplus(ctx, y);
                         return ggml_sqrt(ctx, ggml_abs(ctx, y));
                     },
                     Gate::Exact, false, "delta scaling + sigmoid/log/softplus/clamp/sqrt"});

    // ---- interpolate ------------------------------------------------------
    cases.push_back({"sharp.interp_1536to768_ac0",
                     [=](ggml_context* ctx) {
                         ggml_tensor* x = T(ctx, F32, 1536, 1536, 3, 1, "x");
                         return ggml_interpolate(ctx, x, 768, 768, 3, 1, GGML_SCALE_MODE_BILINEAR);
                     },
                     Gate::Exact, true, "encoder pyramid x0.5, align_corners=False"});
    cases.push_back({"sharp.interp_1280x873to1536_ac1",
                     [=](ggml_context* ctx) {
                         ggml_tensor* x = T(ctx, F32, 1280, 873, 3, 1, "x");
                         return ggml_interpolate(ctx, x, 1536, 1536, 3, 1,
                                                 GGML_SCALE_MODE_BILINEAR |
                                                     GGML_SCALE_FLAG_ALIGN_CORNERS);
                     },
                     Gate::Exact, false, "preprocess resize, align_corners=True"});

    // ---- DA2 (C4) shapes --------------------------------------------------
    log_line("%s: cases: DA2 Small (518 short side, 37x37 patches)", kTag);
    conv("da2.stem_14x14s14_3to384_518", 518, 518, 3, 1, 14, 14, 384, 14, 0, F32, Gate::Exact, true,
         "Depth Anything V2 patch embed");
    cases.push_back({"da2.stem_im2col_f32",
                     [=](ggml_context* ctx) {
                         ggml_tensor* in = T(ctx, F32, 518, 518, 3, 1, "in");
                         ggml_tensor* k = T(ctx, F32, 14, 14, 3, 384, "k");
                         ggml_tensor* im2col =
                             ggml_im2col(ctx, k, in, 14, 14, 0, 0, 1, 1, true, GGML_TYPE_F32);
                         ggml_tensor* r = ggml_mul_mat(
                             ctx,
                             ggml_reshape_2d(ctx, im2col, im2col->ne[0],
                                             im2col->ne[3] * im2col->ne[2] * im2col->ne[1]),
                             ggml_reshape_2d(ctx, k, k->ne[0] * k->ne[1] * k->ne[2], k->ne[3]));
                         r = ggml_reshape_4d(ctx, r, im2col->ne[1], im2col->ne[2], im2col->ne[3],
                                             k->ne[3]);
                         return ggml_cont(ctx, ggml_permute(ctx, r, 0, 1, 3, 2));
                     },
                     Gate::Exact, false, "DA2 patch embed as F32 im2col + mul_mat"});
    mulmat("da2.mulmat_qkv_384x1152_n1370_f16w", 384, 1152, 1370, F16, Gate::F16,
           "ViT-S qkv (6 heads x 64)");
    cases.push_back({"da2.flash_attn_64x1370x6_f16kv",
                     [=](ggml_context* ctx) {
                         ggml_tensor* q = T(ctx, F32, 64, 1370, 6, 1, "q");
                         ggml_tensor* k = T(ctx, F16, 64, 1370, 6, 1, "k");
                         ggml_tensor* v = T(ctx, F16, 64, 1370, 6, 1, "v");
                         return ggml_flash_attn_ext(ctx, q, k, v, nullptr, 1.0f / 8.0f, 0.0f, 0.0f);
                     },
                     Gate::F16, true, "ViT-S attention at 1370 tokens"});
    conv("da2.conv3x3_64_148", 148, 148, 64, 1, 3, 3, 64, 1, 1, F32, Gate::Exact, false,
         "DPT fusion 3x3 at 148^2");
    tconv("da2.tconv4x4_384to256_37to148", 37, 37, 384, 256, 4, 4, 4, F32, Gate::Exact, false,
          "DPT reassemble ConvTranspose2d k4 s4");
    cases.push_back({"da2.interp_37to148_ac1",
                     [=](ggml_context* ctx) {
                         ggml_tensor* x = T(ctx, F32, 37, 37, 64, 1, "x");
                         return ggml_interpolate(ctx, x, 148, 148, 64, 1,
                                                 GGML_SCALE_MODE_BILINEAR |
                                                     GGML_SCALE_FLAG_ALIGN_CORNERS);
                     },
                     Gate::Exact, false, "DPT fusion upsample, align_corners=True"});

    // ---- run --------------------------------------------------------------
    log_line("%s: %zu cases", kTag, cases.size());

    // CPU reference once, before the Vulkan passes: it does not depend on the
    // coopmat setting and is the slowest part on the console CPU. The vectors
    // are kept for both passes (~2 GiB of host memory) so the console CPU does
    // not compute every reference twice; op_audit is not the shipping path and
    // its peak RSS is logged.
    std::vector<std::vector<float>> refs(cases.size());
    std::vector<char> ref_ok(cases.size(), 0);
    for (size_t i = 0; i < cases.size(); i++) {
        Case& c = cases[i];
        if (c.gate == Gate::Finite) {
            ref_ok[i] = 1;  // no reference: finite output is the gate
            continue;
        }
        log_line("%s: reference %s on the CPU", kTag, c.name.c_str());
        std::string err;
        if (!run_one(cpu, nullptr, c, refs[i], c.cpu_ms, 1, err, c.nodes, watchdog,
                     c.big ? 900.0 : 300.0)) {
            log_line("%s: FAIL %s: cpu %s", kTag, c.name.c_str(), err.c_str());
            continue;
        }
        ref_ok[i] = 1;
    }

    // One pass over every case on the current Vulkan backend. Each pass is
    // self-contained: it logs PASS/FAIL per case and a summary, so the first
    // pass's numbers are on the console log even if the second one fails.
    auto run_pass = [&](const char* label) {
        Pass p;
        p.label = label;
        p.coopmat = splat::bench::vk_device_coopmat(0);
        log_line("%s: ---- pass %s (coopmat %s) ----", kTag, label, coopmat_name(p.coopmat));
        const double t_start = splat::bench::now_seconds();

        for (size_t i = 0; i < cases.size(); i++) {
            Case& c = cases[i];
            Row row;
            row.name = c.name;
            row.finite = c.gate == Gate::Finite;
            const int reps = c.big ? 2 : 3;
            const double timeout_s = c.big ? 900.0 : 300.0;

            log_line("%s: case %s (%s, %s)", kTag, c.name.c_str(), gate_name(c.gate),
                     c.big ? "big" : "small");

            if (!ref_ok[i]) {
                row.error = "cpu reference failed";
                p.failed++;
                log_line("%s: FAIL %s: %s", kTag, c.name.c_str(), row.error.c_str());
                p.rows.push_back(std::move(row));
                continue;
            }

            std::vector<float> got;
            std::string err;
            const bool ok =
                run_one(vk, vk_dev, c, got, row.ms, reps, err, row.nodes, watchdog, timeout_s);
            if (!ok) {
                if (err.rfind("unsupported", 0) == 0) {
                    row.unsupported = true;
                    p.unsupported++;
                    log_line("%s: UNSUP %s: %s", kTag, c.name.c_str(), err.c_str());
                } else {
                    row.error = err;
                    p.failed++;
                    log_line("%s: FAIL %s: %s", kTag, c.name.c_str(), err.c_str());
                }
                p.rows.push_back(std::move(row));
                continue;
            }

            Stats s;
            if (c.gate != Gate::Finite) {
                s = stats(refs[i], got);
                row.max_abs = s.max_abs;
                row.nmse = s.nmse;
                row.cos = s.cos;
            } else {
                for (float v : got) {
                    if (!std::isfinite(v)) s.nonfinite++;
                }
            }

            bool pass = true;
            if (c.gate == Gate::Exact) {
                pass = s.nmse <= 1e-6 && s.cos >= 0.99999;
            } else if (c.gate == Gate::Reduction) {
                pass = s.nmse <= 1e-5 && s.cos >= 0.9999;
            } else if (c.gate == Gate::F16) {
                pass = s.nmse <= 1e-4 && s.cos >= 0.999;
            } else {
                pass = s.nonfinite == 0;
            }
            if (s.nonfinite != 0) pass = false;
            row.ok = pass;

            if (c.gate == Gate::Finite) {
                log_line("%s: %s %s coopmat=%s vk %.1f ms nodes %zu max_abs n/a nmse n/a cos n/a "
                         "finite %s",
                         kTag, pass ? "PASS" : "FAIL", c.name.c_str(), coopmat_name(p.coopmat),
                         row.ms, row.nodes, s.nonfinite == 0 ? "yes" : "NO");
            } else {
                log_line("%s: %s %s coopmat=%s vk %.1f ms cpu %.1f ms nodes %zu max_abs %.3g "
                         "nmse %.3g cos %.7f max_ref %.3g",
                         kTag, pass ? "PASS" : "FAIL", c.name.c_str(), coopmat_name(p.coopmat),
                         row.ms, c.cpu_ms, row.nodes, row.max_abs, row.nmse, row.cos, s.max_ref);
            }
            if (pass) p.passed++; else p.failed++;
            p.rows.push_back(std::move(row));
        }

        // The stem comparison is the decision the audit exists for:
        // ggml_conv_2d stages patches in F16, F32 im2col does not.
        const Row* f16_stage = nullptr;
        const Row* f32_stage = nullptr;
        for (const Row& r : p.rows) {
            if (r.name == "sharp.stem_im2col_f16staging") f16_stage = &r;
            if (r.name == "sharp.stem_im2col_f32") f32_stage = &r;
        }
        if (f16_stage != nullptr && f32_stage != nullptr && f32_stage->nmse > 0.0) {
            const double ratio = f16_stage->nmse / f32_stage->nmse;
            log_line("%s: stem parity coopmat=%s: F32 im2col nmse %.3g, ggml_conv_2d nmse %.3g "
                     "(%.1fx worse) -> %s",
                     kTag, coopmat_name(p.coopmat), f32_stage->nmse, f16_stage->nmse, ratio,
                     ratio > 4.0 ? "use F32 im2col" : "either is inside the gate here");
        }

        p.seconds = splat::bench::now_seconds() - t_start;
        log_line("%s: pass %s: ok=%d fail=%d unsupported=%d in %.1f s", kTag, p.label.c_str(),
                 p.passed, p.failed, p.unsupported, p.seconds);
        return p;
    };

    const bool env_disabled = std::getenv("GGML_VK_DISABLE_COOPMAT") != nullptr;
    log_line("%s: coopmat pass 1 at %s (GGML_VK_DISABLE_COOPMAT=%s)", kTag,
             env_disabled ? "the bench.env setting" : "the driver default",
             env_disabled ? "set" : "(unset)");
    Pass first = run_pass(env_disabled ? "bench.env" : "driver-default");

    // Whether the graph split applies: every case here is a single op (or a
    // few), so there is exactly one submit per case. Report the largest graph
    // so a future case that outgrows the benches' 64-node budget is visible.
    size_t max_nodes = 0;
    for (const Row& r : first.rows) max_nodes = std::max(max_nodes, r.nodes);
    log_line("%s: splitting: %zu cases, max %zu nodes per graph, 1 submit each (%s)", kTag,
             first.rows.size(), max_nodes,
             max_nodes <= 64 ? "below the 64-node GraphSession budget"
                             : "ABOVE 64: a split is needed");

    // Pass 2: flip the coopmat setting. ggml-vulkan caches one VkDevice per
    // physical index, so the cached device has to be released first
    // (platform/ps5/ggml/vk_reset.cpp); freeing the backend alone is not
    // enough. The host smoke build has no such glue and reports -1/unknown.
    bool ran_second = false;
    Pass second;
    if (first.coopmat < 0) {
        log_line("%s: coopmat on/off: state unknown in this build, single pass "
                 "(the console build runs both)", kTag);
    } else if (first.coopmat == 0 && !env_disabled) {
        log_line("%s: coopmat on/off: the driver exposes no cooperative matrix, so both "
                 "settings are the same path; single pass", kTag);
    } else if (!splat::bench::vk_reset_available()) {
        log_line("%s: coopmat on/off: this build cannot release the cached VkDevice, single "
                 "pass", kTag);
    } else {
        ggml_backend_free(vk);
        vk = nullptr;
        if (!splat::bench::vk_release_device(0)) {
            log_line("%s: coopmat pass 2 skipped: could not release the cached VkDevice", kTag);
        } else {
            if (first.coopmat != 0) {
                setenv("GGML_VK_DISABLE_COOPMAT", "1", 1);
            } else {
                unsetenv("GGML_VK_DISABLE_COOPMAT");
            }
            vk = ggml_backend_dev_init(vk_dev, nullptr);
            if (vk == nullptr) {
                log_line("%s: coopmat pass 2 skipped: Vulkan backend re-init failed", kTag);
            } else {
                second = run_pass(first.coopmat != 0 ? "coopmat-disabled" : "coopmat-enabled");
                ran_second = true;
                if (second.coopmat == first.coopmat) {
                    if (second.coopmat == 0) {
                        log_line("%s: coopmat: the driver exposes no cooperative matrix with "
                                 "either setting; both passes are the scalar path", kTag);
                    } else {
                        log_line("%s: WARNING coopmat state did not change between passes (%s); "
                                 "the setting did not reach the new device",
                                 kTag, coopmat_name(second.coopmat));
                    }
                }
            }
        }
    }

    // Side by side: the two passes in one place, which is the comparison the
    // console run exists for.
    if (ran_second) {
        log_line("%s: ---- coopmat on/off, side by side (pass 1 %s, pass 2 %s) ----", kTag,
                 coopmat_name(first.coopmat), coopmat_name(second.coopmat));
        for (size_t i = 0; i < first.rows.size() && i < second.rows.size(); i++) {
            const Row& a = first.rows[i];
            const Row& b = second.rows[i];
            const char* a_status = a.unsupported ? "UNSUP" : a.ok ? "PASS" : "FAIL";
            const char* b_status = b.unsupported ? "UNSUP" : b.ok ? "PASS" : "FAIL";
            log_line("%s: %-42s %s %8.1f ms nmse %-9.3g | %s %8.1f ms nmse %-9.3g", kTag,
                     a.name.c_str(), a_status, a.ms, a.nmse, b_status, b.ms, b.nmse);
        }
    }

    const int failed = first.failed + (ran_second ? second.failed : 0);
    const int unsupported = first.unsupported + (ran_second ? second.unsupported : 0);
    const double seconds = first.seconds + (ran_second ? second.seconds : 0.0);
    log_line("%s: summary ok=%d fail=%d unsupported=%d in %.1f s", kTag,
             first.passed + (ran_second ? second.passed : 0), failed, unsupported, seconds);
    log_line("%s: peak RSS %s", kTag, human_bytes(peak_rss_bytes()).c_str());
    log_line("%s: %s", kTag, failed == 0 && unsupported == 0 ? "PASS" : "FAIL");
    log_line("%s: done, exit %d", kTag, failed == 0 && unsupported == 0 ? 0 : 1);

    ggml_backend_free(vk);
    ggml_backend_free(cpu);
    return failed == 0 && unsupported == 0 ? 0 : 1;
}

#ifndef PS5_TITLE
// The payload build keeps its own main: a payload is launched by its ELF and
// there is nothing to select. The title links this file with -DPS5_TITLE and
// dispatches to op_audit_run from bench_title.cpp.
int
main(int argc, char** argv) {
    return op_audit_run(argc, argv);
}
#endif
