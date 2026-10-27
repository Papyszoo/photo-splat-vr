// SHARP-shaped ggml op audit: build each op at the shapes SHARP uses at
// 1536x1536, check ggml-vulkan supports_op, run on Vulkan (CPU reference where
// affordable) and report max abs / NMSE / cosine.
//
// Not part of the repo. Build against third_party/ggml v0.27.0.

#include "ggml.h"
#include "ggml-alloc.h"
#include "ggml-backend.h"
#include "ggml-cpu.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

static uint64_t hash_str(const std::string & s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

struct case_t {
    std::string name;
    std::function<ggml_tensor *(ggml_context *)> build;
    bool parity;   // run CPU reference and compare (skip for > ~200 GFLOP)
    bool big;      // SHARP's real dimensions (may be slow / large)
    std::string note;
};

static ggml_tensor * T(ggml_context * ctx, ggml_type type, int64_t a, int64_t b, int64_t c, int64_t d, const char * name) {
    ggml_tensor * t = ggml_new_tensor_4d(ctx, type, a, b, c, d);
    ggml_set_name(t, name);
    return t;
}

static void fill_leaf(ggml_tensor * t) {
    const size_t n = ggml_nelements(t);
    std::vector<uint8_t> buf(ggml_nbytes(t));
    std::mt19937 rng((uint32_t) hash_str(std::string(ggml_get_name(t))));
    std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
    if (t->type == GGML_TYPE_F32) {
        float * p = (float *) buf.data();
        for (size_t i = 0; i < n; i++) p[i] = dist(rng);
    } else if (t->type == GGML_TYPE_F16) {
        ggml_fp16_t * p = (ggml_fp16_t *) buf.data();
        for (size_t i = 0; i < n; i++) p[i] = ggml_fp32_to_fp16(dist(rng));
    } else if (t->type == GGML_TYPE_I32) {
        int32_t * p = (int32_t *) buf.data();
        for (size_t i = 0; i < n; i++) p[i] = (int32_t) (rng() % 8);
    } else {
        fprintf(stderr, "unhandled leaf type %d\n", (int) t->type);
        abort();
    }
    ggml_backend_tensor_set(t, buf.data(), 0, buf.size());
}

static bool run_one(ggml_backend_t backend, ggml_backend_dev_t dev, const case_t & c,
                    std::vector<float> & out, double & ms, std::string & err) {
    ggml_init_params ip = { /*mem_size*/ 512u * 1024u * 1024u, /*mem_buffer*/ nullptr, /*no_alloc*/ true };
    ggml_context * ctx = ggml_init(ip);
    if (!ctx) { err = "ggml_init"; return false; }

    ggml_tensor * out_t = c.build(ctx);
    ggml_cgraph * gf = ggml_new_graph(ctx);
    ggml_build_forward_expand(gf, out_t);

    std::string unsup;
    if (backend != nullptr && dev != nullptr) {
        for (int i = 0; i < ggml_graph_n_nodes(gf); i++) {
            ggml_tensor * n = ggml_graph_node(gf, i);
            if (!ggml_backend_dev_supports_op(dev, n)) {
                char b[256];
                snprintf(b, sizeof b, "%s[%lld,%lld,%lld,%lld];", ggml_op_desc(n),
                         (long long) n->ne[0], (long long) n->ne[1], (long long) n->ne[2], (long long) n->ne[3]);
                unsup += b;
            }
        }
    }
    if (!unsup.empty()) { err = "unsupported: " + unsup; ggml_free(ctx); return false; }

    ggml_backend_buffer_t buf = ggml_backend_alloc_ctx_tensors(ctx, backend);
    if (!buf) { err = "alloc"; ggml_free(ctx); return false; }

    for (ggml_tensor * t = ggml_get_first_tensor(ctx); t; t = ggml_get_next_tensor(ctx, t)) {
        if (t->op == GGML_OP_NONE && t->view_src == nullptr && t != out_t) fill_leaf(t);
    }

    const int reps = (backend == nullptr) ? 1 : 3;
    std::vector<double> times;
    for (int r = 0; r < reps; r++) {
        ggml_backend_synchronize(backend);
        auto t0 = std::chrono::steady_clock::now();
        ggml_status st = ggml_backend_graph_compute(backend, gf);
        ggml_backend_synchronize(backend);
        auto t1 = std::chrono::steady_clock::now();
        if (st != GGML_STATUS_SUCCESS) { err = std::string("compute: ") + ggml_status_to_string(st); ggml_backend_buffer_free(buf); ggml_free(ctx); return false; }
        times.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::sort(times.begin(), times.end());
    ms = times[times.size() / 2];

    out.resize(ggml_nelements(out_t));
    ggml_backend_tensor_get(out_t, out.data(), 0, out.size() * sizeof(float));
    ggml_backend_buffer_free(buf);
    ggml_free(ctx);
    return true;
}

static void stats(const std::vector<float> & a, const std::vector<float> & b, double & max_abs, double & nmse, double & cos) {
    max_abs = 0; double se = 0, ss = 0, dot = 0, na = 0, nb = 0; size_t nan = 0;
    for (size_t i = 0; i < a.size(); i++) {
        double d = (double) a[i] - (double) b[i];
        if (!std::isfinite(a[i]) || !std::isfinite(b[i])) nan++;
        max_abs = std::max(max_abs, std::fabs(d));
        se += d * d; ss += (double) a[i] * a[i];
        dot += (double) a[i] * b[i]; na += (double) a[i] * a[i]; nb += (double) b[i] * b[i];
    }
    nmse = (ss > 0) ? se / ss : se;
    cos = (na > 0 && nb > 0) ? dot / std::sqrt(na * nb) : 1.0;
    if (nan) nmse = 1e9;
}

int main(int argc, char ** argv) {
    std::string only;
    bool list = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else if (!strcmp(argv[i], "--list")) list = true;
    }

    ggml_backend_load_all();
    ggml_backend_dev_t cpu_dev = ggml_backend_dev_by_name("CPU");
    ggml_backend_dev_t vk_dev  = ggml_backend_dev_by_name("Vulkan0");
    if (!cpu_dev || !vk_dev) { fprintf(stderr, "backends missing (cpu=%p vk=%p)\n", (void*)cpu_dev, (void*)vk_dev); return 2; }
    ggml_backend_t cpu = ggml_backend_dev_init(cpu_dev, nullptr);
    ggml_backend_t vk  = ggml_backend_dev_init(vk_dev, nullptr);

    fprintf(stderr, "Vulkan device: %s\n", ggml_backend_dev_description(vk_dev));

    const ggml_type F32 = GGML_TYPE_F32, F16 = GGML_TYPE_F16;
    std::vector<case_t> cases;

    auto conv = [&](const char * nm, int64_t W, int64_t H, int64_t Cin, int64_t N,
                    int64_t KW, int64_t KH, int64_t Cout, int s, int p, ggml_type kt, bool par, bool big, const char * note) {
        cases.push_back({ nm, [=](ggml_context * ctx) {
            ggml_tensor * in = T(ctx, F32, W, H, Cin, N, "in");
            ggml_tensor * k  = T(ctx, kt, KW, KH, Cin, Cout, "k");
            return ggml_conv_2d_direct(ctx, k, in, s, s, p, p, 1, 1);
        }, par, big, note });
    };
    auto tconv = [&](const char * nm, int64_t W, int64_t H, int64_t Cin, int64_t Cout, ggml_type kt, bool par, bool big, const char * note) {
        cases.push_back({ nm, [=](ggml_context * ctx) {
            ggml_tensor * in = T(ctx, F32, W, H, Cin, 1, "in");
            ggml_tensor * k  = T(ctx, kt, 2, 2, Cout, Cin, "k");
            return ggml_conv_transpose_2d_p0(ctx, k, in, 2);
        }, par, big, note });
    };
    auto mulmat = [&](const char * nm, int64_t K, int64_t M, int64_t N, ggml_type wt, bool par, const char * note) {
        cases.push_back({ nm, [=](ggml_context * ctx) {
            ggml_tensor * w = T(ctx, wt, K, M, 1, 1, "w");
            ggml_tensor * x = T(ctx, F32, K, N, 1, 1, "x");
            return ggml_mul_mat(ctx, w, x);
        }, par, false, note });
    };

    // ---- encoder ----
    conv("conv_stem_16x16_s16_384x384x3x35", 384, 384, 3, 35, 16, 16, 1024, 16, 0, F32, true,  true,  "patch_embed, 35 patches");
    conv("conv_stem_16x16_s16_384x384x3x35_f16w", 384, 384, 3, 35, 16, 16, 1024, 16, 0, F16, true, false, "f16 weights");
    conv("conv3x3_1024_96", 96, 96, 1024, 1, 3, 3, 1024, 1, 1, F32, true, true, "SPN / ViT tap projection");
    conv("conv3x3_256_768", 768, 768, 256, 1, 3, 3, 256, 1, 1, F32, false, true, "DPT convs[1] at full 768^2");
    conv("conv3x3_128_768", 768, 768, 128, 1, 3, 3, 128, 1, 1, F32, false, true, "texture/geometry head");
    conv("conv1x1_2048_1024_48", 48, 48, 2048, 1, 1, 1, 1024, 1, 0, F32, true, false, "fuse_lowres");
    conv("conv2x2_s2_5_128_1536", 1536, 1536, 5, 1, 2, 2, 128, 2, 0, F32, false, true, "Gaussian decoder skip conv");
    conv("conv3x3_256_96_parity", 96, 96, 256, 1, 3, 3, 256, 1, 1, F32, true, false, "parity proxy for 768^2 convs");
    tconv("tconv2x2_1024_48_96", 48, 48, 1024, 1024, F32, true, false, "SPN upsample1");
    tconv("tconv2x2_256_192_384", 192, 192, 256, 256, F32, true, false, "parity proxy for SPN 384->768");
    tconv("tconv2x2_256_384_768", 384, 384, 256, 256, F32, false, true, "SPN upsample_latent0 step");
    tconv("tconv2x2_128_768_1536", 768, 768, 128, 128, F32, false, true, "disparity head 768^2 -> 1536^2");

    cases.push_back({ "layernorm_1024_577_35", [=](ggml_context * ctx) {
        ggml_tensor * x = T(ctx, F32, 1024, 577, 35, 1, "x");
        return ggml_norm(ctx, x, 1e-6f);
    }, true, true, "ViT LayerNorm, 35 patches x 577 tokens" });

    cases.push_back({ "gelu_erf_4096_20195", [=](ggml_context * ctx) {
        ggml_tensor * x = T(ctx, F32, 4096, 577 * 35, 1, 1, "x");
        return ggml_gelu_erf(ctx, x);
    }, true, true, "ViT MLP hidden, exact erf GELU" });

    mulmat("mulmat_qkv_1024x3072_n20195_f32w", 1024, 3072, 577 * 35, F32, true,  "qkv, f32 weights");
    mulmat("mulmat_qkv_1024x3072_n20195_f16w", 1024, 3072, 577 * 35, F16, true,  "qkv, f16 weights");
    mulmat("mulmat_proj_1024x1024_n20195_f16w", 1024, 1024, 577 * 35, F16, true, "attn proj / fc2");

    auto attn = [&](const char * nm, ggml_type kvt, bool par, const char * note) {
        cases.push_back({ nm, [=](ggml_context * ctx) {
            ggml_tensor * q = T(ctx, F32, 64, 577, 16, 35, "q");
            ggml_tensor * k = T(ctx, kvt, 64, 577, 16, 35, "k");
            ggml_tensor * v = T(ctx, kvt, 64, 577, 16, 35, "v");
            return ggml_flash_attn_ext(ctx, q, k, v, nullptr, 1.0f / 8.0f, 0.0f, 0.0f);
        }, par, true, note });
    };
    attn("flash_attn_64x577x16x35_f16kv", F16, true, "ViT-L attention, 35 patches");
    attn("flash_attn_64x577x16x35_f32kv", F32, true, "ViT-L attention, f32 K/V");

    cases.push_back({ "softmax_577x577x16", [=](ggml_context * ctx) {
        ggml_tensor * x = T(ctx, F32, 577, 577, 16, 1, "x");
        return ggml_soft_max(ctx, x);
    }, true, false, "non-fused attention fallback" });

    cases.push_back({ "conv_im2col_f16_staging_stem", [=](ggml_context * ctx) {
        ggml_tensor * in = T(ctx, F32, 384, 384, 3, 35, "in");
        ggml_tensor * k  = T(ctx, F32, 16, 16, 3, 1024, "k");
        return ggml_conv_2d(ctx, k, in, 16, 16, 0, 0, 1, 1);   // F16 im2col staging
    }, true, false, "ggml_conv_2d (im2col F16) vs CONV_2D direct: accuracy cost" });

    cases.push_back({ "conv_im2col_f32_stem", [=](ggml_context * ctx) {
        ggml_tensor * in = T(ctx, F32, 384, 384, 3, 35, "in");
        ggml_tensor * k  = T(ctx, F32, 16, 16, 3, 1024, "k");
        ggml_tensor * im2col = ggml_im2col(ctx, k, in, 16, 16, 0, 0, 1, 1, true, GGML_TYPE_F32);
        ggml_tensor * r = ggml_mul_mat(ctx,
            ggml_reshape_2d(ctx, im2col, im2col->ne[0], im2col->ne[3] * im2col->ne[2] * im2col->ne[1]),
            ggml_reshape_2d(ctx, k, k->ne[0] * k->ne[1] * k->ne[2], k->ne[3]));
        r = ggml_reshape_4d(ctx, r, im2col->ne[1], im2col->ne[2], im2col->ne[3], k->ne[3]);
        return ggml_cont(ctx, ggml_permute(ctx, r, 0, 1, 3, 2));
    }, true, false, "F32 im2col + mul_mat: exact replacement for ggml_conv_2d" });

    // ---- depth decoder / gaussian decoder ----
    cases.push_back({ "groupnorm_8g_128_768", [=](ggml_context * ctx) {
        ggml_tensor * x = T(ctx, F32, 768, 768, 128, 1, "x");
        return ggml_group_norm(ctx, x, 8, 1e-5f);
    }, false, true, "Gaussian decoder head GN (8 groups)" });
    cases.push_back({ "groupnorm_8g_128_96_parity", [=](ggml_context * ctx) {
        ggml_tensor * x = T(ctx, F32, 96, 96, 128, 1, "x");
        return ggml_group_norm(ctx, x, 8, 1e-5f);
    }, true, false, "parity proxy" });

    cases.push_back({ "pool_max_2x2_1536x2", [=](ggml_context * ctx) {
        ggml_tensor * x = T(ctx, F32, 1536, 1536, 2, 1, "x");
        return ggml_pool_2d(ctx, x, GGML_OP_POOL_MAX, 2, 2, 2, 2, 0, 0);
    }, true, true, "surface_min" });
    cases.push_back({ "pool_avg_2x2_1536x3", [=](ggml_context * ctx) {
        ggml_tensor * x = T(ctx, F32, 1536, 1536, 3, 1, "x");
        return ggml_pool_2d(ctx, x, GGML_OP_POOL_AVG, 2, 2, 2, 2, 0, 0);
    }, true, true, "colour base avg_pool" });

    // ---- elementwise / composer ----
    cases.push_back({ "add_bcast_1024x577x35", [=](ggml_context * ctx) {
        ggml_tensor * a = T(ctx, F32, 1024, 577, 35, 1, "a");
        ggml_tensor * b = T(ctx, F32, 1024, 1, 1, 1, "b");
        return ggml_add(ctx, a, b);
    }, true, false, "LayerScale / bias broadcast" });

    cases.push_back({ "composer_head_tail_14x2x768", [=](ggml_context * ctx) {
        ggml_tensor * d = T(ctx, F32, 14, 2, 768, 768, "d");     // deltas
        ggml_tensor * b = T(ctx, F32, 14, 1, 1, 1, "b");          // base value broadcast
        ggml_tensor * y = ggml_add(ctx, d, b);
        y = ggml_scale(ctx, y, 1e-3f);
        y = ggml_sigmoid(ctx, y);
        y = ggml_clamp(ctx, y, 0.01f, 0.99f);
        y = ggml_log(ctx, y);
        y = ggml_softplus(ctx, y);
        return ggml_sqrt(ctx, ggml_abs(ctx, y));
    }, true, false, "delta scaling + activations (sigmoid/log/softplus/clamp)" });

    // ---- interpolate ----
    cases.push_back({ "interp_1536_to_768_ac0", [=](ggml_context * ctx) {
        ggml_tensor * x = T(ctx, F32, 1536, 1536, 3, 1, "x");
        return ggml_interpolate(ctx, x, 768, 768, 3, 1, GGML_SCALE_MODE_BILINEAR);
    }, true, true, "encoder pyramid x0.5, align_corners=False" });
    cases.push_back({ "interp_1536_to_384_ac0", [=](ggml_context * ctx) {
        ggml_tensor * x = T(ctx, F32, 1536, 1536, 3, 1, "x");
        return ggml_interpolate(ctx, x, 384, 384, 3, 1, GGML_SCALE_MODE_BILINEAR);
    }, true, false, "pyramid x0.25" });
    cases.push_back({ "interp_1280x873_to_1536_ac1", [=](ggml_context * ctx) {
        ggml_tensor * x = T(ctx, F32, 1280, 873, 3, 1, "x");
        return ggml_interpolate(ctx, x, 1536, 1536, 3, 1, GGML_SCALE_MODE_BILINEAR | GGML_SCALE_FLAG_ALIGN_CORNERS);
    }, true, false, "preprocess resize, align_corners=True, anisotropic" });

    printf("%-46s %-9s %8s %8s %10s %10s %9s  %s\n",
           "case", "vk", "nodes", "ms_vk", "max_abs", "nmse", "cos", "note");
    fflush(stdout);

    int pass = 0, fail = 0, unsup = 0;
    for (const case_t & c : cases) {
        if (!only.empty() && c.name.find(only) == std::string::npos) continue;
        if (list) { printf("%s\n", c.name.c_str()); continue; }

        std::vector<float> ref, got;
        double cpu_ms = 0, vk_ms = 0;
        std::string err;

        if (c.parity) {
            if (!run_one(cpu, nullptr, c, ref, cpu_ms, err)) { printf("%-46s CPU-FAIL  %s\n", c.name.c_str(), err.c_str()); fail++; fflush(stdout); continue; }
        }
        bool ok = run_one(vk, vk_dev, c, got, vk_ms, err);
        if (!ok) {
            printf("%-46s %-9s %s  %s\n", c.name.c_str(), err.rfind("unsupported", 0) == 0 ? "UNSUP" : "FAIL", "", err.c_str());
            if (err.rfind("unsupported", 0) == 0) unsup++; else fail++;
            fflush(stdout);
            continue;
        }
        double max_abs = 0, nmse = 0, cos = 1;
        char sa[32] = "n/a", sn[32] = "n/a", sc[32] = "n/a";
        if (c.parity) {
            stats(ref, got, max_abs, nmse, cos);
            snprintf(sa, sizeof sa, "%.3g", max_abs);
            snprintf(sn, sizeof sn, "%.3g", nmse);
            snprintf(sc, sizeof sc, "%.6f", cos);
        } else {
            size_t nonfinite = 0; double s = 0;
            for (float v : got) { if (!std::isfinite(v)) nonfinite++; s += v; }
            if (nonfinite) { printf("%-46s FAIL      non-finite values: %zu\n", c.name.c_str(), nonfinite); fail++; fflush(stdout); continue; }
            max_abs = s; // keep compiler quiet
        }
        pass++;
        printf("%-46s %-9s %8s %8.2f %10s %10s %9s  %s\n",
               c.name.c_str(), "OK", "", vk_ms, sa, sn, sc, c.note.c_str());
        fflush(stdout);
    }
    printf("\nsummary: ok=%d fail=%d unsupported=%d\n", pass, fail, unsup);
    ggml_backend_free(vk);
    ggml_backend_free(cpu);
    return fail ? 1 : 0;
}
