#include "recon/ggml/weights.h"

#include <cstring>
#include <stdexcept>
#include <utility>

#include "ggml-alloc.h"
#include "ggml-backend.h"
#include "ggml.h"
#include "gguf.h"

namespace splat {
namespace {

ggml_tensor* new_tensor_like(ggml_context* ctx, const ggml_tensor* source, const std::string& name) {
    ggml_tensor* tensor = ggml_new_tensor(ctx, source->type, 4, source->ne);
    if (tensor == nullptr) throw std::runtime_error("cannot create GGUF tensor '" + name + "'");
    ggml_set_name(tensor, name.c_str());
    return tensor;
}

}  // namespace

void GgmlWeights::reset() {
    if (weight_buffer_ != nullptr) ggml_backend_buffer_free(weight_buffer_);
    weight_buffer_ = nullptr;
    if (ctx_ != nullptr) ggml_free(ctx_);
    ctx_ = nullptr;
    if (gguf_ != nullptr) gguf_free(static_cast<gguf_context*>(const_cast<void*>(gguf_)));
    gguf_ = nullptr;
    tensors_.clear();
    strings_.clear();
    backend_ = nullptr;
    tensor_bytes_ = 0;
}

GgmlWeights::~GgmlWeights() { reset(); }

GgmlWeights::GgmlWeights(GgmlWeights&& other) noexcept { *this = std::move(other); }

GgmlWeights& GgmlWeights::operator=(GgmlWeights&& other) noexcept {
    if (this == &other) return *this;
    reset();
    backend_ = other.backend_;
    ctx_ = other.ctx_;
    weight_buffer_ = other.weight_buffer_;
    tensors_ = std::move(other.tensors_);
    strings_ = std::move(other.strings_);
    gguf_ = other.gguf_;
    architecture_ = std::move(other.architecture_);
    tensor_bytes_ = other.tensor_bytes_;
    other.backend_ = nullptr;
    other.ctx_ = nullptr;
    other.weight_buffer_ = nullptr;
    other.gguf_ = nullptr;
    other.tensor_bytes_ = 0;
    return *this;
}

Result<GgmlWeights> GgmlWeights::load(const Path& gguf_path, GgmlBackend& backend) {
    GgmlWeights weights;
    weights.backend_ = &backend;

    ggml_context* source = nullptr;
    gguf_init_params params{};
    params.no_alloc = false;  // load the tensor blob, then copy to the backend
    params.ctx = &source;
    gguf_context* gguf = gguf_init_from_file(gguf_path.string().c_str(), params);
    if (gguf == nullptr || source == nullptr) {
        if (gguf != nullptr) gguf_free(gguf);
        return Result<GgmlWeights>::failure("cannot read GGUF " + gguf_path.string());
    }
    weights.gguf_ = gguf;

    const int64_t count = gguf_get_n_tensors(gguf);
    if (count <= 0) {
        ggml_free(source);
        weights.reset();
        return Result<GgmlWeights>::failure(gguf_path.string() + ": GGUF has no tensors");
    }

    ggml_init_params context_params{};
    context_params.mem_size = static_cast<size_t>(count + 16) * ggml_tensor_overhead() + (1u << 20);
    context_params.mem_buffer = nullptr;
    context_params.no_alloc = true;
    weights.ctx_ = ggml_init(context_params);
    if (weights.ctx_ == nullptr) {
        ggml_free(source);
        weights.reset();
        return Result<GgmlWeights>::failure("cannot create the weight tensor context");
    }

    for (int64_t index = 0; index < count; ++index) {
        const char* name = gguf_get_tensor_name(gguf, index);
        const ggml_tensor* from_file = ggml_get_tensor(source, name);
        if (from_file == nullptr) {
            ggml_free(source);
            weights.reset();
            return Result<GgmlWeights>::failure("GGUF tensor '" + std::string(name) + "' is not addressable");
        }
        ggml_tensor* tensor = new_tensor_like(weights.ctx_, from_file, name);
        weights.tensors_[name] = tensor;
        weights.tensor_bytes_ += ggml_nbytes(tensor);
    }

    weights.weight_buffer_ = ggml_backend_alloc_ctx_tensors(weights.ctx_, backend.handle());
    if (weights.weight_buffer_ == nullptr) {
        ggml_free(source);
        weights.reset();
        return Result<GgmlWeights>::failure("cannot allocate " + std::to_string(weights.tensor_bytes_) +
                                            " bytes of GGUF weights on " + backend.describe());
    }

    for (int64_t index = 0; index < count; ++index) {
        const char* name = gguf_get_tensor_name(gguf, index);
        const ggml_tensor* from_file = ggml_get_tensor(source, name);
        ggml_tensor* tensor = weights.tensors_[name];
        if (ggml_nbytes(tensor) != ggml_nbytes(from_file)) {
            return Result<GgmlWeights>::failure(std::string("GGUF tensor '") + name + "' changed size while loading");
        }
        ggml_backend_tensor_set(tensor, from_file->data, 0, ggml_nbytes(tensor));
    }

    // Metadata: keep the string keys; the arrays (tensor name lists) are large
    // and nothing at runtime reads them.
    const int64_t kv_count = gguf_get_n_kv(gguf);
    for (int64_t index = 0; index < kv_count; ++index) {
        if (gguf_get_kv_type(gguf, index) != GGUF_TYPE_STRING) continue;
        const char* key = gguf_get_key(gguf, index);
        weights.strings_[key] = gguf_get_val_str(gguf, index);
    }
    const auto architecture = weights.strings_.find("general.architecture");
    if (architecture != weights.strings_.end()) weights.architecture_ = architecture->second;

    ggml_free(source);
    return Result<GgmlWeights>::success(std::move(weights));
}

ggml_tensor* GgmlWeights::find(const std::string& name) const {
    const auto it = tensors_.find(name);
    return it == tensors_.end() ? nullptr : it->second;
}

ggml_tensor* GgmlWeights::need(const std::string& name) const {
    ggml_tensor* tensor = find(name);
    if (tensor == nullptr) throw std::runtime_error("model is missing GGUF tensor '" + name + "'");
    return tensor;
}

std::string GgmlWeights::metadata(const std::string& key) const {
    const auto it = strings_.find(key);
    return it == strings_.end() ? std::string() : it->second;
}

Status GgmlWeights::upload(ggml_tensor* tensor, const void* data, size_t bytes) {
    if (tensor == nullptr) return Status::error("cannot upload into a null tensor");
    if (bytes != ggml_nbytes(tensor)) {
        return Status::error("upload size " + std::to_string(bytes) + " does not match tensor '" +
                             std::string(tensor->name) + "' (" + std::to_string(ggml_nbytes(tensor)) + ")");
    }
    ggml_backend_tensor_set(tensor, data, 0, bytes);
    return Status::success();
}

Status GgmlWeights::download(const ggml_tensor* tensor, std::vector<float>* values) const {
    if (tensor == nullptr) return Status::error("cannot download from a null tensor");
    values->resize(static_cast<size_t>(ggml_nelements(tensor)));
    if (tensor->type == GGML_TYPE_F32) {
        ggml_backend_tensor_get(tensor, values->data(), 0, ggml_nbytes(tensor));
        return Status::success();
    }
    if (tensor->type == GGML_TYPE_F16) {
        // fp16 weights (position embeddings, in the shipped GGUF) are read back
        // for host-side maths; the public result is always float32.
        std::vector<uint16_t> halves(values->size());
        ggml_backend_tensor_get(tensor, halves.data(), 0, ggml_nbytes(tensor));
        for (size_t i = 0; i < halves.size(); ++i) (*values)[i] = ggml_fp16_to_fp32(halves[i]);
        return Status::success();
    }
    return Status::error("only float32 and float16 tensors can be read back");
}

}  // namespace splat
