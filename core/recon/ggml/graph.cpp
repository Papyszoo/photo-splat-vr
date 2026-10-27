#include "recon/ggml/graph.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "ggml-alloc.h"
#include "ggml-backend.h"
#include "ggml.h"

namespace splat {
namespace {

// Metadata budget for one submit segment, derived from its node budget: a few
// hundred bytes per tensor, twice as many tensors as nodes, plus the graph
// arrays. Segment memories are kept alive until the session ends, so this is a
// per-submit cost rather than a peak (28 segments for a DA2 photo).

}  // namespace

GraphSession::GraphSession(GgmlBackend& backend, GgmlWeights& weights, size_t nodes_per_submit,
                           size_t arena_bytes)
    : backend_(&backend),
      weights_(&weights),
      nodes_per_submit_(std::max<size_t>(1, nodes_per_submit)),
      arena_bytes_(arena_bytes == 0 ? (384u << 20) : arena_bytes) {
    if (!backend.ok()) throw std::runtime_error("GraphSession needs an initialized backend");
    gallocr_ = ggml_gallocr_new(backend.buffer_type());
    if (gallocr_ == nullptr) throw std::runtime_error("cannot create the graph allocator");
    arena_ = ggml_backend_alloc_buffer(backend.handle(), arena_bytes_);
    if (arena_ == nullptr) throw std::runtime_error("cannot allocate the persistent tensor arena");
    persistent_alloc_ = new ggml_tallocr(ggml_tallocr_new(arena_));
    ggml_init_params params{};
    // Metadata only; the arena holds the data. Room for a few thousand
    // cross-submit tensors (one per carry).
    params.mem_size = (2u << 20) + 4096 * ggml_tensor_overhead();
    params.mem_buffer = nullptr;
    params.no_alloc = true;
    persistent_ctx_ = ggml_init(params);
    if (persistent_ctx_ == nullptr) throw std::runtime_error("cannot create the persistent tensor context");
    new_segment();
}

GraphSession::~GraphSession() {
    for (ggml_context* segment : segments_) ggml_free(segment);
    segments_.clear();
    if (gallocr_ != nullptr) ggml_gallocr_free(gallocr_);
    gallocr_ = nullptr;
    delete persistent_alloc_;
    persistent_alloc_ = nullptr;
    if (arena_ != nullptr) ggml_backend_buffer_free(arena_);
    arena_ = nullptr;
    if (persistent_ctx_ != nullptr) ggml_free(persistent_ctx_);
    persistent_ctx_ = nullptr;
}

void GraphSession::new_segment() {
    // A segment can hold at most one graph budget's worth of nodes; the capacity
    // has slack because the caller may postpone a checkpoint across a stage.
    const size_t capacity = std::max<size_t>(512, nodes_per_submit_ * 8);
    ggml_init_params params{};
    params.mem_size = ggml_graph_overhead_custom(capacity, /*grads=*/false) +
                      2 * capacity * ggml_tensor_overhead() + (256u << 10);
    params.mem_buffer = nullptr;
    params.no_alloc = true;
    ctx_ = ggml_init(params);
    if (ctx_ == nullptr) throw std::runtime_error("cannot create a graph segment context");
    graph_ = ggml_new_graph_custom(ctx_, capacity, /*grads=*/false);
    if (graph_ == nullptr) throw std::runtime_error("cannot create a graph segment");
    segments_.push_back(ctx_);
}

ggml_tensor* GraphSession::persistent(const std::string& name, std::vector<int64_t> ne) {
    while (ne.size() < 4) ne.push_back(1);
    ggml_tensor* tensor = ggml_new_tensor_4d(persistent_ctx_, GGML_TYPE_F32, ne[0], ne[1], ne[2], ne[3]);
    if (tensor == nullptr) throw std::runtime_error("cannot create persistent tensor '" + name + "'");
    ggml_set_name(tensor, name.c_str());
    if (ggml_tallocr_alloc(persistent_alloc_, tensor) != GGML_STATUS_SUCCESS) {
        throw std::runtime_error("persistent tensor arena exhausted at '" + name + "' (" +
                                 std::to_string(arena_used_) + " of " + std::to_string(arena_bytes_) +
                                 " bytes used)");
    }
    arena_used_ += ggml_nbytes(tensor);
    return tensor;
}

ggml_tensor* GraphSession::checkpoint(ggml_tensor* t) {
    if (t == nullptr) throw std::runtime_error("GraphSession::checkpoint got a null tensor");
    ggml_build_forward_expand(graph_, t);
    if (static_cast<size_t>(ggml_graph_n_nodes(graph_)) < nodes_per_submit_) return t;
    return carry(t);
}

ggml_tensor* GraphSession::carry(ggml_tensor* t) {
    if (t == nullptr) throw std::runtime_error("GraphSession::carry got a null tensor");
    ggml_tensor* destination = persistent(std::string(t->name) + ".carried",
                                          {t->ne[0], t->ne[1], t->ne[2], t->ne[3]});
    ggml_tensor* copy = ggml_cpy(ctx_, t, destination);
    const Status submitted = submit(copy);
    if (!submitted.ok()) throw std::runtime_error(submitted.message());
    return destination;
}

ggml_tensor* GraphSession::finish(ggml_tensor* t) { return carry(t); }

Status GraphSession::submit(ggml_tensor* output) {
    ggml_build_forward_expand(graph_, output);
    const int node_count = ggml_graph_n_nodes(graph_);
    if (node_count > 0) {
        if (!ggml_gallocr_alloc_graph(gallocr_, graph_)) {
            return Status::error("cannot allocate the graph segment (" +
                                 std::to_string(node_count) + " nodes)");
        }
        const ggml_status status = ggml_backend_graph_compute(backend_->handle(), graph_);
        if (status != GGML_STATUS_SUCCESS) {
            return Status::error(std::string("graph submit failed: ") + ggml_status_to_string(status) +
                                 " after " + std::to_string(submits_) + " submits");
        }
        ++submits_;
    }
    new_segment();
    return Status::success();
}

Status GraphSession::download(const ggml_tensor* tensor, std::vector<float>* values) const {
    return weights_->download(tensor, values);
}

size_t GraphSession::arena_used() const { return arena_used_; }
size_t GraphSession::arena_size() const { return arena_bytes_; }

}  // namespace splat
