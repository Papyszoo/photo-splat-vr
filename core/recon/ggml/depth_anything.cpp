#include "recon/ggml/depth_anything.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "recon/ggml/graph.h"
#include "recon/ggml/nn.h"

namespace splat {
namespace {

constexpr int kImageMean[3] = {485, 456, 406};  // 0.485 / 0.456 / 0.406, scaled by 1000
constexpr int kImageStd[3] = {229, 224, 225};

// The reference's Resize(width=518, height=518, keep_aspect_ratio=True,
// ensure_multiple_of=14, resize_method="lower_bound"): scale so the *short*
// side is at least 518, then round both sides to a multiple of 14.
void reference_size(int width, int height, int input_size, int* out_width, int* out_height) {
    const double scale_height = static_cast<double>(input_size) / height;
    const double scale_width = static_cast<double>(input_size) / width;
    double scale = 0.0;
    if (scale_width > scale_height) {
        scale = scale_width;
    } else {
        scale = scale_height;
    }
    auto constrain = [&](double value) {
        double rounded = std::round(value / 14.0) * 14.0;
        if (rounded < input_size) rounded = std::ceil(value / 14.0) * 14.0;
        return static_cast<int>(rounded);
    };
    *out_width = constrain(scale * width);
    *out_height = constrain(scale * height);
}

double cubic_weight(double t) {
    constexpr double a = -0.75;
    t = std::fabs(t);
    if (t < 1.0) return ((a + 2.0) * t - (a + 3.0)) * t * t + 1.0;
    if (t < 2.0) return ((a * t - 5.0 * a) * t + 8.0 * a) * t - 4.0 * a;
    return 0.0;
}

int clamp_index(int value, int size) { return std::min(std::max(value, 0), size - 1); }

}  // namespace

Result<std::vector<float>> resize_cubic_rgb(const uint8_t* rgb, int width, int height, int out_width,
                                            int out_height) {
    using ResultT = Result<std::vector<float>>;
    if (rgb == nullptr || width <= 0 || height <= 0 || out_width <= 0 || out_height <= 0) {
        return ResultT::failure("resize_cubic_rgb: invalid size");
    }
    // cv2 INTER_CUBIC: half-pixel mapping, scale = in/out, cubic a = -0.75,
    // edge indices clamped. No antialiasing on downscale, same as cv2.
    const double step_x = static_cast<double>(width) / out_width;
    const double step_y = static_cast<double>(height) / out_height;
    std::vector<float> out(static_cast<size_t>(out_width) * out_height * 3, 0.0f);
    std::vector<int> index_x(static_cast<size_t>(out_width) * 4);
    std::vector<double> weight_x(static_cast<size_t>(out_width) * 4);

    for (int x = 0; x < out_width; ++x) {
        const double source = (x + 0.5) * step_x - 0.5;
        const int base = static_cast<int>(std::floor(source));
        const double fraction = source - base;
        for (int k = 0; k < 4; ++k) {
            index_x[static_cast<size_t>(x) * 4 + k] = clamp_index(base - 1 + k, width);
            weight_x[static_cast<size_t>(x) * 4 + k] = cubic_weight(fraction - (k - 1));
        }
    }

    for (int y = 0; y < out_height; ++y) {
        const double source = (y + 0.5) * step_y - 0.5;
        const int base = static_cast<int>(std::floor(source));
        const double fraction = source - base;
        int rows[4];
        double weights[4];
        for (int k = 0; k < 4; ++k) {
            rows[k] = clamp_index(base - 1 + k, height);
            weights[k] = cubic_weight(fraction - (k - 1));
        }
        for (int x = 0; x < out_width; ++x) {
            float* destination = out.data() + (static_cast<size_t>(y) * out_width + x) * 3;
            for (int channel = 0; channel < 3; ++channel) {
                double sum = 0.0;
                for (int ky = 0; ky < 4; ++ky) {
                    const uint8_t* row = rgb + (static_cast<size_t>(rows[ky]) * width) * 3;
                    double horizontal = 0.0;
                    for (int kx = 0; kx < 4; ++kx) {
                        const int sx = index_x[static_cast<size_t>(x) * 4 + kx];
                        horizontal += weight_x[static_cast<size_t>(x) * 4 + kx] * row[sx * 3 + channel];
                    }
                    sum += weights[ky] * horizontal;
                }
                destination[channel] = static_cast<float>(sum);
            }
        }
    }
    return ResultT::success(std::move(out));
}

Result<std::vector<float>> da2_preprocess(const uint8_t* rgb, int width, int height, int* net_width,
                                          int* net_height, int input_size) {
    using ResultT = Result<std::vector<float>>;
    int out_width = 0;
    int out_height = 0;
    reference_size(width, height, input_size, &out_width, &out_height);
    if (out_width % 14 != 0 || out_height % 14 != 0) {
        return ResultT::failure("da2_preprocess: resized sides are not a multiple of 14");
    }
    Result<std::vector<float>> resized = resize_cubic_rgb(rgb, width, height, out_width, out_height);
    if (!resized.ok()) return resized;

    // (x / 255 - mean) / std, CHW layout.
    std::vector<float> chw(static_cast<size_t>(3) * out_width * out_height);
    const std::vector<float>& hwc = resized.value();
    for (int channel = 0; channel < 3; ++channel) {
        const float mean = static_cast<float>(kImageMean[channel]) / 1000.0f;
        const float stddev = static_cast<float>(kImageStd[channel]) / 1000.0f;
        float* plane = chw.data() + static_cast<size_t>(channel) * out_width * out_height;
        for (size_t pixel = 0; pixel < static_cast<size_t>(out_width) * out_height; ++pixel) {
            plane[pixel] = (hwc[pixel * 3 + static_cast<size_t>(channel)] / 255.0f - mean) / stddev;
        }
    }
    *net_width = out_width;
    *net_height = out_height;
    return ResultT::success(std::move(chw));
}

DepthAnythingV2::~DepthAnythingV2() = default;

Status DepthAnythingV2::initialize(const Path& gguf, bool prefer_vulkan, size_t nodes_per_submit,
                                  size_t arena_bytes) {
    std::string note;
    Result<GgmlBackend> backend = GgmlBackend::create(prefer_vulkan, &note);
    if (!backend.ok()) return Status::error(backend.message());
    backend_ = std::move(backend.value());
    nodes_per_submit_ = nodes_per_submit;
    arena_bytes_ = arena_bytes;

    Result<GgmlWeights> weights = GgmlWeights::load(gguf, backend_);
    if (!weights.ok()) return Status::error(weights.message());
    weights_ = std::move(weights.value());

    const std::string architecture = weights_.architecture();
    if (architecture != "depth-anything-v2") {
        return Status::error(gguf.string() + ": architecture '" + architecture +
                             "', expected 'depth-anything-v2' (convert with --backend da2)");
    }

    // Small only: 384 dim, 6 heads, 12 blocks, 14 px patches, taps 2/5/8/11.
    vit_config_ = ViTConfig{};
    vit_config_.combined_qkv = false;
    vit_config_.taps = {2, 5, 8, 11};
    vit_names_ = ViTNames{};

    dpt_config_ = DptConfig{};
    dpt_config_.in_channels = {{vit_config_.embed_dim, vit_config_.embed_dim,
                               vit_config_.embed_dim, vit_config_.embed_dim}};
    dpt_config_.reassemble_channels = {{48, 96, 192, 384}};
    dpt_names_ = DptNames{};

    const std::string blocks = weights_.metadata("da2.blocks");
    if (!blocks.empty() && std::stoi(blocks) != vit_config_.blocks) {
        return Status::error("GGUF reports " + blocks + " ViT blocks, the DA2-Small config expects " +
                             std::to_string(vit_config_.blocks));
    }
    const std::string embed_dim = weights_.metadata("da2.embed_dim");
    if (!embed_dim.empty() && std::stoi(embed_dim) != vit_config_.embed_dim) {
        return Status::error("GGUF reports embed_dim " + embed_dim + ", expected " +
                             std::to_string(vit_config_.embed_dim));
    }

    try {
        vit_weights_ = resolve_vit_weights(weights_, vit_config_, vit_names_);
        dpt_weights_ = resolve_dpt_weights(weights_, dpt_config_, dpt_names_);
    } catch (const std::exception& error) {
        return Status::error(std::string("GGUF does not match the DA2-Small graph: ") + error.what());
    }
    const int64_t positions = vit_weights_.pos_embed->ne[1] - 1;
    position_grid_ = static_cast<int>(std::lround(std::sqrt(static_cast<double>(positions))));
    if (static_cast<int64_t>(position_grid_) * position_grid_ != positions ||
        vit_weights_.pos_embed->ne[0] != vit_config_.embed_dim) {
        return Status::error("position embedding is not a square patch grid of embed_dim");
    }
    return Status::success();
}

Result<std::unique_ptr<DepthAnythingV2>> DepthAnythingV2::create(const Path& gguf, bool prefer_vulkan,
                                                                size_t nodes_per_submit,
                                                                size_t arena_bytes) {
    using ResultT = Result<std::unique_ptr<DepthAnythingV2>>;
    auto model = std::unique_ptr<DepthAnythingV2>(new DepthAnythingV2());
    const Status status = model->initialize(gguf, prefer_vulkan, nodes_per_submit, arena_bytes);
    if (!status.ok()) return ResultT::failure(status.message());
    return ResultT::success(std::move(model));
}

Result<DepthAnythingResult> DepthAnythingV2::run(const float* input_chw, int width, int height,
                                                const DepthAnythingStageCallback& stage_cb,
                                                Progress* progress) {
    using ResultT = Result<DepthAnythingResult>;
    const auto started = std::chrono::steady_clock::now();
    if (input_chw == nullptr) return ResultT::failure("run() got a null input");
    if (width % vit_config_.patch_size != 0 || height % vit_config_.patch_size != 0) {
        return ResultT::failure("input size is not a multiple of the patch size");
    }
    const int patch_w = width / vit_config_.patch_size;
    const int patch_h = height / vit_config_.patch_size;

    try {
        GraphSession session(backend_, weights_, nodes_per_submit_, arena_bytes_);

        auto emit = [&](const std::string& name, ggml_tensor* tensor) {
            if (!stage_cb) return;
            DepthAnythingStage stage;
            stage.name = name;
            stage.shape = {tensor->ne[0], tensor->ne[1], tensor->ne[2], tensor->ne[3]};
            const Status status = session.download(tensor, &stage.values);
            if (!status.ok()) throw std::runtime_error(status.message());
            stage_cb(stage);
        };

        // Input: (W, H, 3, 1) channels-last, kept in the persistent arena so the
        // first submit can consume it.
        ggml_tensor* image = session.persistent("da2_input", {width, height, 3, 1});
        Status status = weights_.upload(image, input_chw,
                                        static_cast<size_t>(3) * width * height * sizeof(float));
        if (!status.ok()) return ResultT::failure(status.message());

        if (progress != nullptr) progress->report(0.05f, "patch embed");
        ggml_tensor* tokens = vit_patch_embed(session, vit_weights_, vit_config_, image);
        tokens = session.carry(tokens);
        emit("patch_embed", tokens);

        tokens = vit_add_position(session, weights_, vit_weights_, tokens, position_grid_,
                                  position_grid_, patch_w, patch_h);
        tokens = session.carry(tokens);

        std::vector<ggml_tensor*> taps(vit_config_.taps.size(), nullptr);
        for (int block = 0; block < vit_config_.blocks; ++block) {
            tokens = vit_block(session, vit_weights_.blocks[static_cast<size_t>(block)], vit_config_,
                               tokens);
            tokens = session.carry(tokens);
            for (size_t tap = 0; tap < vit_config_.taps.size(); ++tap) {
                if (vit_config_.taps[tap] != block) continue;
                char name[32];
                std::snprintf(name, sizeof(name), "block_%02d", block);
                emit(name, tokens);
                taps[tap] = tokens;
            }
            if (progress != nullptr) {
                progress->report(0.1f + 0.5f * static_cast<float>(block + 1) /
                                            static_cast<float>(vit_config_.blocks),
                                 "encoder");
            }
        }
        for (ggml_tensor* tap : taps) {
            if (tap == nullptr) return ResultT::failure("a configured tap layer was never reached");
        }

        // Taps: final LayerNorm, drop the cls column, then a channels-last map
        // for the DPT neck.
        std::vector<ggml_tensor*> maps;
        maps.reserve(taps.size());
        for (size_t tap = 0; tap < taps.size(); ++tap) {
            ggml_tensor* normed = vit_final_norm(session, vit_weights_, vit_config_.norm_eps, taps[tap]);
            ggml_tensor* patch_tokens =
                ggml_view_2d(session.ctx(), normed, vit_config_.embed_dim, patch_w * patch_h,
                             normed->nb[1], static_cast<size_t>(vit_config_.embed_dim) * sizeof(float));
            patch_tokens = session.carry(patch_tokens);
            char name[32];
            std::snprintf(name, sizeof(name), "tap_patch_%02d", static_cast<int>(tap));
            emit(name, patch_tokens);
            maps.push_back(nn::tokens_to_map(session.ctx(), patch_tokens, patch_w, patch_h));
        }

        if (progress != nullptr) progress->report(0.6f, "dpt");
        DptOutput dpt = dpt_forward(session, dpt_weights_, dpt_config_, maps, patch_w, patch_h,
                                    static_cast<bool>(stage_cb));
        if (stage_cb) {
            for (size_t level = 0; level < dpt.layers.size(); ++level) {
                char name[32];
                std::snprintf(name, sizeof(name), "layer_%02d", static_cast<int>(level));
                emit(name, dpt.layers[level]);
            }
            for (size_t path = 0; path < dpt.paths.size(); ++path) {
                char name[32];
                std::snprintf(name, sizeof(name), "path_%d",
                              static_cast<int>(dpt.paths.size() - path));
                emit(name, dpt.paths[path]);
            }
            emit("conv1", dpt.conv1);
        }

        DepthAnythingResult result;
        result.width = width;
        result.height = height;
        const Status downloaded = session.download(dpt.depth, &result.depth);
        if (!downloaded.ok()) return ResultT::failure(downloaded.message());
        result.submits = session.submits();
        result.arena_bytes = session.arena_used();
        result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        emit("depth", dpt.depth);
        if (progress != nullptr) progress->report(1.0f, "depth");
        return ResultT::success(std::move(result));
    } catch (const std::exception& error) {
        return ResultT::failure(std::string("DA2 inference failed: ") + error.what());
    }
}

}  // namespace splat
