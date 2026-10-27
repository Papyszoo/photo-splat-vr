#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "common/path.h"
#include "common/status.h"
#include "recon/ggml/backend.h"

struct ggml_context;
struct ggml_tensor;
struct ggml_backend_buffer;
struct ggml_tallocr;

namespace splat {

// GGUF weights copied into a backend buffer.
//
// Names follow what the C1 converters write: the DA2 profile strips the HF
// module prefixes, and the SHARP profile drops the torch prefix, so both are
// plain "blocks.0.attn.qkv.weight" style keys.
class GgmlWeights {
public:
    GgmlWeights() = default;
    ~GgmlWeights();

    GgmlWeights(const GgmlWeights&) = delete;
    GgmlWeights& operator=(const GgmlWeights&) = delete;
    GgmlWeights(GgmlWeights&& other) noexcept;
    GgmlWeights& operator=(GgmlWeights&& other) noexcept;

    // Reads the GGUF, creates a matching tensor set and copies every tensor to
    // the backend.
    static Result<GgmlWeights> load(const Path& gguf, GgmlBackend& backend);

    // nullptr when the GGUF does not have the tensor.
    ggml_tensor* find(const std::string& name) const;
    // Throws std::runtime_error when the tensor is missing. Graph code is
    // wrapped in one try/catch at the model boundary instead of checking every
    // lookup; a missing tensor means the GGUF profile does not match the model.
    ggml_tensor* need(const std::string& name) const;

    // Copies host bytes into a tensor created above (or any backend tensor).
    Status upload(ggml_tensor* tensor, const void* data, size_t bytes);
    // Reads a backend tensor back into host floats (ne layout, row-major).
    Status download(const ggml_tensor* tensor, std::vector<float>* values) const;

    const std::string& architecture() const { return architecture_; }
    std::string metadata(const std::string& key) const;
    uint64_t tensor_bytes() const { return tensor_bytes_; }
    size_t tensor_count() const { return tensors_.size(); }
    GgmlBackend& backend() { return *backend_; }
    const GgmlBackend& backend() const { return *backend_; }

private:
    void reset();

    GgmlBackend* backend_ = nullptr;
    ggml_context* ctx_ = nullptr;                 // weight tensor set, one buffer
    ggml_backend_buffer* weight_buffer_ = nullptr;
    std::map<std::string, ggml_tensor*> tensors_;
    std::map<std::string, std::string> strings_;
    const void* gguf_ = nullptr;  // gguf_context, freed with the weights
    std::string architecture_;
    uint64_t tensor_bytes_ = 0;
};

}  // namespace splat
