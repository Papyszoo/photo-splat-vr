#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "common/status.h"
#include "ggml.h"
#include "recon/ggml/backend.h"
#include "recon/ggml/weights.h"

struct ggml_gallocr;
typedef struct ggml_gallocr * ggml_gallocr_t;
struct ggml_tallocr;

namespace splat {

// Runs one forward pass as a sequence of small graph submits instead of a
// single giant one. Spec risk: "long GPU submissions hit a timeout (device
// lost)" — RADV loses the device on long compute jobs (llama.cpp #21724), and
// ggml-vulkan's own submit splitting is backend-internal. Here the *graph* is
// split: every submit is one ggml_backend_graph_compute() call, and tensors
// that cross a submit boundary are copied into a host-managed arena that
// outlives the per-submit allocation.
//
// Usage:
//   GraphSession session(backend, nodes_per_submit, arena_bytes);
//   ggml_tensor* x = ...;                       // ops built in session.ctx()
//   x = session.checkpoint(x);                  // submit only when over budget
//   x = session.carry(x);                       // force a submit boundary
//   Result<std::vector<float>> out = ...;       // copy a carried tensor to host
//   session.finish(x);                          // last submit
//
// `checkpoint` keeps the graph below `nodes_per_submit`; `carry` is for stage
// boundaries where the caller wants a stable, inspectable tensor (parity tests,
// DPT stages). Both return a tensor living in the arena, safe to use as an
// input to later submits.
class GraphSession {
public:
    GraphSession(GgmlBackend& backend, GgmlWeights& weights, size_t nodes_per_submit = 64,
                 size_t arena_bytes = 0);
    ~GraphSession();

    GraphSession(const GraphSession&) = delete;
    GraphSession& operator=(const GraphSession&) = delete;

    ggml_context* ctx() const { return ctx_; }
    ggml_tensor* persistent(const std::string& name, std::vector<int64_t> ne);

    // Submits the ops built since the last submit when the segment reached the
    // node budget, otherwise returns `t` unchanged.
    ggml_tensor* checkpoint(ggml_tensor* t);
    // Always submits and returns a copy of `t` in the arena.
    ggml_tensor* carry(ggml_tensor* t);
    // Finishes the pass: submits the remaining ops and returns the final copy.
    ggml_tensor* finish(ggml_tensor* t);

    // Reads a tensor back to host floats (ne order, row-major).
    Status download(const ggml_tensor* tensor, std::vector<float>* values) const;

    size_t submits() const { return submits_; }
    size_t nodes() const { return graph_ == nullptr ? 0 : ggml_graph_n_nodes(graph_); }
    size_t nodes_per_submit() const { return nodes_per_submit_; }
    size_t arena_used() const;
    size_t arena_size() const;

private:
    Status submit(ggml_tensor* output);
    void new_segment();

    GgmlBackend* backend_ = nullptr;
    GgmlWeights* weights_ = nullptr;
    size_t nodes_per_submit_ = 64;
    ggml_context* ctx_ = nullptr;
    ggml_cgraph* graph_ = nullptr;
    ggml_gallocr_t gallocr_ = nullptr;
    ggml_context* persistent_ctx_ = nullptr;
    ggml_tallocr* persistent_alloc_ = nullptr;
    ggml_backend_buffer* arena_ = nullptr;
    size_t arena_bytes_ = 0;
    size_t arena_used_ = 0;
    size_t submits_ = 0;
    std::vector<ggml_context*> segments_;  // freed with the session
};

}  // namespace splat
