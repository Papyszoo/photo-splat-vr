#include "model/da2_profile.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <vector>

#include "common/half.h"
#include "model/gguf_writer.h"
#include "model/sha256.h"
#include <nlohmann/json.hpp>

namespace splat {
namespace {

constexpr size_t kChunkBytes = 1u << 20;  // floats converted per read

std::string basename_of(const Path& path) { return path.filename().string(); }

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string lowercase(std::string text) {
    for (char& character : text) {
        if (character >= 'A' && character <= 'F') character = static_cast<char>(character - 'A' + 'a');
    }
    return text;
}

struct SafeTensor {
    std::string name;         // HF key
    std::string gguf_name;
    std::vector<int64_t> shape;  // outermost dimension first, as stored
    uint64_t offset = 0;         // inside the data section
    uint64_t bytes = 0;
    bool fp16 = true;
};

struct SafeTensorsHeader {
    uint64_t data_start = 0;
    std::vector<SafeTensor> tensors;
    std::vector<std::string> dropped;
};

Result<SafeTensorsHeader> read_header(const Path& path, const ConvertOptions& options,
                                      std::string* source_sha256) {
    SafeTensorsHeader header;
    std::FILE* file = std::fopen(path.string().c_str(), "rb");
    if (file == nullptr) return Result<SafeTensorsHeader>::failure("cannot open " + path.string());

    struct Closer {
        std::FILE* file;
        ~Closer() { std::fclose(file); }
    } closer{file};

    uint64_t header_bytes = 0;
    if (std::fread(&header_bytes, 1, sizeof(header_bytes), file) != sizeof(header_bytes)) {
        return Result<SafeTensorsHeader>::failure(path.string() + ": truncated safetensors header length");
    }
    if (header_bytes == 0 || header_bytes > (64u << 20)) {
        return Result<SafeTensorsHeader>::failure(path.string() + ": implausible safetensors header size " +
                                                  std::to_string(header_bytes));
    }
    std::string text(header_bytes, '\0');
    if (std::fread(text.data(), 1, header_bytes, file) != header_bytes) {
        return Result<SafeTensorsHeader>::failure(path.string() + ": truncated safetensors header");
    }
    header.data_start = 8 + header_bytes;

    nlohmann::json json;
    try {
        json = nlohmann::json::parse(text);
    } catch (const std::exception& error) {
        return Result<SafeTensorsHeader>::failure(path.string() + ": malformed safetensors JSON: " +
                                                  error.what());
    }
    if (!json.is_object()) {
        return Result<SafeTensorsHeader>::failure(path.string() + ": safetensors header is not an object");
    }

    for (auto it = json.begin(); it != json.end(); ++it) {
        const std::string& name = it.key();
        if (name == "__metadata__") continue;
        const nlohmann::json& info = it.value();
        if (!info.contains("dtype") || !info.contains("shape") || !info.contains("data_offsets")) {
            return Result<SafeTensorsHeader>::failure(path.string() + ": tensor '" + name +
                                                      "' is missing dtype/shape/data_offsets");
        }
        const std::string dtype = info["dtype"].get<std::string>();
        if (dtype != "F32") {
            return Result<SafeTensorsHeader>::failure(path.string() + ": tensor '" + name + "' is " +
                                                      dtype + "; only F32 is converted");
        }
        SafeTensor tensor;
        tensor.name = name;
        for (const nlohmann::json& dim : info["shape"]) {
            const int64_t value = dim.get<int64_t>();
            if (value <= 0) {
                return Result<SafeTensorsHeader>::failure(path.string() + ": tensor '" + name +
                                                          "' has a non-positive dimension");
            }
            tensor.shape.push_back(value);
        }
        const uint64_t begin = info["data_offsets"][0].get<uint64_t>();
        const uint64_t end = info["data_offsets"][1].get<uint64_t>();
        if (end < begin) {
            return Result<SafeTensorsHeader>::failure(path.string() + ": tensor '" + name +
                                                      "' has inverted data offsets");
        }
        tensor.offset = begin;
        tensor.bytes = end - begin;
        uint64_t elements = 1;
        for (int64_t dim : tensor.shape) elements *= static_cast<uint64_t>(dim);
        if (tensor.bytes != elements * 4) {
            return Result<SafeTensorsHeader>::failure(path.string() + ": tensor '" + name + "' holds " +
                                                      std::to_string(tensor.bytes) + " bytes, expected " +
                                                      std::to_string(elements * 4));
        }
        if (is_da2_dead_tensor(name)) {
            header.dropped.push_back(name);
            continue;
        }
        if (tensor.shape.size() > 4) {
            return Result<SafeTensorsHeader>::failure("tensor '" + name + "' has rank " +
                                                      std::to_string(tensor.shape.size()) +
                                                      "; GGUF converts ranks 1..4");
        }
        tensor.gguf_name = da2_gguf_tensor_name(name);
        tensor.fp16 = tensor.shape.size() >= 2 && !options.keep_fp32;
        header.tensors.push_back(std::move(tensor));
    }

    if (header.tensors.empty()) {
        return Result<SafeTensorsHeader>::failure(path.string() + ": no convertible tensors");
    }

    // The digest is always computed: it goes into the GGUF metadata, and rule 2
    // additionally requires it to match the pinned checkpoint.
    const Result<std::string> digest = sha256_file(path);
    if (!digest.ok()) return Result<SafeTensorsHeader>::failure(digest.message());
    *source_sha256 = lowercase(digest.value());
    if (options.verify_sha256 && *source_sha256 != kDepthAnythingV2SmallSha256) {
        return Result<SafeTensorsHeader>::failure(
            path.string() + ": SHA-256 " + *source_sha256 + " does not match the pinned " +
            kDepthAnythingV2SmallSha256 + " (spec rule 2); refusing to convert");
    }
    return Result<SafeTensorsHeader>::success(std::move(header));
}

}  // namespace

bool is_da2_dead_tensor(const std::string& hf_name) {
    return hf_name == "backbone.embeddings.mask_token";
}

std::string da2_gguf_tensor_name(const std::string& hf_name) {
    if (hf_name == "backbone.embeddings.cls_token") return "cls_token";
    if (hf_name == "backbone.embeddings.position_embeddings") return "pos_embed";
    if (starts_with(hf_name, "backbone.embeddings.patch_embeddings.projection.")) {
        return "patch_embed.proj." + hf_name.substr(std::strlen("backbone.embeddings.patch_embeddings.projection."));
    }
    if (starts_with(hf_name, "backbone.layernorm.")) {
        return "norm." + hf_name.substr(std::strlen("backbone.layernorm."));
    }
    if (starts_with(hf_name, "backbone.encoder.layer.")) {
        const std::string rest = hf_name.substr(std::strlen("backbone.encoder.layer."));
        const size_t dot = rest.find('.');
        const std::string index = rest.substr(0, dot);
        const std::string tail = rest.substr(dot + 1);
        const std::string prefix = "blocks." + index + ".";
        if (tail == "attention.attention.query.weight") return prefix + "attn.q.weight";
        if (tail == "attention.attention.query.bias") return prefix + "attn.q.bias";
        if (tail == "attention.attention.key.weight") return prefix + "attn.k.weight";
        if (tail == "attention.attention.key.bias") return prefix + "attn.k.bias";
        if (tail == "attention.attention.value.weight") return prefix + "attn.v.weight";
        if (tail == "attention.attention.value.bias") return prefix + "attn.v.bias";
        if (tail == "attention.output.dense.weight") return prefix + "attn.proj.weight";
        if (tail == "attention.output.dense.bias") return prefix + "attn.proj.bias";
        if (tail == "layer_scale1.lambda1") return prefix + "ls1";
        if (tail == "layer_scale2.lambda1") return prefix + "ls2";
        return prefix + tail;  // norm1/norm2/mlp.* keep their names
    }
    if (starts_with(hf_name, "neck.convs.")) return hf_name;  // neck.convs.N.weight/bias
    if (starts_with(hf_name, "neck.reassemble_stage.layers.")) {
        const std::string rest = hf_name.substr(std::strlen("neck.reassemble_stage.layers."));
        const size_t dot = rest.find('.');
        const std::string index = rest.substr(0, dot);
        std::string tail = rest.substr(dot + 1);
        if (starts_with(tail, "projection.")) tail = "proj." + tail.substr(11);
        else if (starts_with(tail, "resize.")) tail = "resize." + tail.substr(7);
        return "reassemble." + index + "." + tail;
    }
    if (starts_with(hf_name, "neck.fusion_stage.layers.")) {
        const std::string rest = hf_name.substr(std::strlen("neck.fusion_stage.layers."));
        const size_t dot = rest.find('.');
        const std::string index = rest.substr(0, dot);
        const std::string tail = rest.substr(dot + 1);
        const std::string prefix = "fusion." + index + ".";
        if (starts_with(tail, "projection.")) return prefix + "out_conv." + tail.substr(11);
        // residual_layer{1,2}.convolution{1,2}.{weight,bias}
        const size_t first = tail.find("residual_layer");
        const size_t second = tail.find("convolution");
        if (first == 0 && second != std::string::npos) {
            const std::string unit = tail.substr(first + 14, 1);
            const std::string conv = tail.substr(second + 11, 1);
            const std::string param = tail.substr(tail.rfind('.') + 1);
            return prefix + "res" + unit + ".conv" + conv + "." + param;
        }
    }
    if (starts_with(hf_name, "head.")) return hf_name;  // head.conv1/2/3.{weight,bias}
    return hf_name;
}

Result<ConvertStats> convert_depth_anything_v2(const Path& in, const Path& out,
                                              const ConvertOptions& options, Progress& progress) {
    const auto started = std::chrono::steady_clock::now();
    ConvertStats stats;

    std::error_code file_error;
    const uintmax_t source_bytes = std::filesystem::file_size(in, file_error);
    if (file_error) {
        return Result<ConvertStats>::failure("cannot stat " + in.string() + ": " + file_error.message());
    }
    stats.source_bytes = source_bytes;

    if (source_bytes != kDepthAnythingV2SmallBytes && options.verify_sha256) {
        return Result<ConvertStats>::failure(
            in.string() + ": " + std::to_string(source_bytes) + " bytes, expected the pinned " +
            std::to_string(kDepthAnythingV2SmallBytes) + " (spec rule 2)");
    }

    Result<SafeTensorsHeader> header = read_header(in, options, &stats.source_sha256);
    if (!header.ok()) return Result<ConvertStats>::failure(header.message());
    progress.report(0.1f, "checkpoint verified");

    std::vector<GgufTensorDesc> descriptions;
    descriptions.reserve(header.value().tensors.size());
    std::vector<std::string> full_names;
    full_names.reserve(header.value().tensors.size());
    for (const SafeTensor& tensor : header.value().tensors) {
        GgufTensorDesc description;
        description.name = tensor.gguf_name;
        description.shape = tensor.shape;
        description.fp16 = tensor.fp16;
        descriptions.push_back(std::move(description));
        full_names.push_back(tensor.name);
        stats.source_tensor_bytes += tensor.bytes;
    }

    GgufWriter writer;
    Status status = writer.begin(out, std::move(descriptions));
    if (!status.ok()) return Result<ConvertStats>::failure(status.message());

    writer.set_string("general.architecture", "depth-anything-v2");
    writer.set_string("general.name", "Depth Anything V2 Small (depth-anything/Depth-Anything-V2-Small-hf)");
    writer.set_string("da2.backend", options.backend);
    writer.set_string("da2.converter", "splat_convert");
    writer.set_u32("da2.converter_version", 1);
    writer.set_string("da2.source_url", kDepthAnythingV2SmallUrl);
    writer.set_string("da2.source_sha256", stats.source_sha256);
    writer.set_string("da2.source_filename", basename_of(in));
    writer.set_u64("da2.source_bytes", stats.source_bytes);
    writer.set_u32("da2.source_tensor_count", kDepthAnythingV2SmallTensorCount);
    writer.set_u32("da2.tensor_count", static_cast<uint32_t>(full_names.size()));
    writer.set_u32("da2.dropped_tensor_count", static_cast<uint32_t>(header.value().dropped.size()));
    writer.set_strings("da2.dropped_tensors", header.value().dropped);
    writer.set_strings("da2.source_tensor_names", full_names);
    writer.set_u32("da2.patch_size", kDepthAnythingV2PatchSize);
    writer.set_u32("da2.embed_dim", 384);
    writer.set_u32("da2.blocks", 12);
    writer.set_u32("da2.heads", 6);
    writer.set_u32("da2.intermediate_layers", 4);
    writer.set_string(
        "da2.tensor_name_rule",
        "HF keys shortened: backbone.encoder.layer.N.* -> blocks.N.*, "
        "neck.reassemble_stage.layers.N.* -> reassemble.N.*, "
        "neck.fusion_stage.layers.N.* -> fusion.N.*, backbone.layernorm -> norm, "
        "backbone.embeddings.* -> patch_embed/pos_embed/cls_token; HF order in da2.source_tensor_names");
    writer.set_strings(
        "da2.changes",
        {"dtype: tensors with 2 or more dimensions converted from float32 to float16",
         "dtype: 1-D tensors (norm weights, norm biases, conv biases, layer scales) kept as float32",
         "layout: safetensors (d0..dn) stored as ggml ne (dn..d0); bytes unchanged, row-major "
         "element order preserved, no transpose applied",
         "converted from the official Hugging Face safetensors checkpoint of "
         "Depth Anything V2 Small (Apache-2.0)"});

    status = writer.write_header();
    if (!status.ok()) return Result<ConvertStats>::failure(status.message());
    stats.data_offset = writer.data_offset();

    std::FILE* file = std::fopen(in.string().c_str(), "rb");
    if (file == nullptr) return Result<ConvertStats>::failure("cannot reopen " + in.string());
    std::vector<float> floats;
    std::vector<uint16_t> halves;
    floats.resize(kChunkBytes / 4);
    halves.resize(kChunkBytes / 4);

    const size_t total = header.value().tensors.size();
    for (size_t index = 0; index < total; ++index) {
        const SafeTensor& tensor = header.value().tensors[index];
        uint64_t expected_bytes = 0;
        status = writer.begin_tensor(&expected_bytes);
        if (!status.ok()) {
            std::fclose(file);
            return Result<ConvertStats>::failure(status.message());
        }
        if (std::fseek(file, static_cast<long>(header.value().data_start + tensor.offset), SEEK_SET) != 0) {
            std::fclose(file);
            return Result<ConvertStats>::failure("cannot seek to tensor '" + tensor.name + "'");
        }
        uint64_t remaining = tensor.bytes;
        while (remaining > 0) {
            const size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, kChunkBytes));
            if (std::fread(floats.data(), 1, want, file) != want) {
                std::fclose(file);
                return Result<ConvertStats>::failure("short read on tensor '" + tensor.name + "'");
            }
            if (tensor.fp16) {
                const size_t count = want / sizeof(float);
                for (size_t i = 0; i < count; ++i) halves[i] = float_to_half(floats[i]);
                status = writer.append_tensor_bytes(halves.data(), count * sizeof(uint16_t));
            } else {
                status = writer.append_tensor_bytes(floats.data(), want);
            }
            if (!status.ok()) {
                std::fclose(file);
                return Result<ConvertStats>::failure(status.message());
            }
            remaining -= want;
        }
        status = writer.end_tensor();
        if (!status.ok()) {
            std::fclose(file);
            return Result<ConvertStats>::failure(status.message());
        }
        progress.report(0.1f + 0.9f * static_cast<float>(index + 1) / static_cast<float>(total),
                        "converting tensors");
    }
    std::fclose(file);

    status = writer.finish();
    if (!status.ok()) return Result<ConvertStats>::failure(status.message());
    stats.gguf_bytes = writer.bytes_written();
    stats.tensor_count = total;
    stats.dropped_count = header.value().dropped.size();
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return Result<ConvertStats>::success(std::move(stats));
}

}  // namespace splat
