// Shared ggml blocks: backend selection and the submit-splitting helper.
//
// No model data is needed; these cases always run. The numeric parity of the
// ViT/DPT blocks against the PyTorch golden dumps lives in
// tests/ggml_da2_parity_test.cpp.

#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

#include "ggml.h"
#include "recon/ggml/backend.h"
#include "recon/ggml/graph.h"
#include "recon/ggml/weights.h"

using splat::GgmlBackend;
using splat::GgmlWeights;
using splat::GraphSession;

TEST_CASE("ggml backend initializes and can be forced to the CPU") {
    splat::Result<GgmlBackend> cpu = GgmlBackend::create(/*prefer_vulkan=*/false);
    REQUIRE_MESSAGE(cpu.ok(), cpu.message());
    CHECK(cpu.value().info().kind == GgmlBackend::Kind::Cpu);
    CHECK(cpu.value().info().name == "CPU");
    CHECK(cpu.value().describe() == "CPU");

    std::string note;
    splat::Result<GgmlBackend> preferred = GgmlBackend::create(/*prefer_vulkan=*/true, &note);
    REQUIRE_MESSAGE(preferred.ok(), preferred.message());
    // Vulkan is optional on the host (MoltenVK, ICDs); either way the backend
    // must come up and usable.
    if (preferred.value().info().kind == GgmlBackend::Kind::Vulkan) {
        MESSAGE("Vulkan device: " << preferred.value().describe());
        CHECK(preferred.value().info().total_memory > 0);
    } else {
        MESSAGE("CPU fallback" << std::string(note.empty() ? "" : ": " + note));
    }
}

TEST_CASE("graph session splits work into several submits") {
    splat::Result<GgmlBackend> backend = GgmlBackend::create(/*prefer_vulkan=*/false);
    REQUIRE_MESSAGE(backend.ok(), backend.message());

    GgmlWeights none;  // only used for download(), which needs no weights
    GraphSession session(backend.value(), none, /*nodes_per_submit=*/4, /*arena_bytes=*/16u << 20);

    std::vector<float> ones(64, 1.0f);
    ggml_tensor* one = session.persistent("one", {8, 8});
    REQUIRE(session.ctx() != nullptr);
    splat::Status uploaded = none.upload(one, ones.data(), ones.size() * sizeof(float));
    REQUIRE_MESSAGE(uploaded.ok(), uploaded.message());

    ggml_tensor* acc = one;
    for (int round = 0; round < 12; ++round) {
        acc = ggml_add(session.ctx(), acc, one);
        acc = session.checkpoint(acc);
        CHECK(session.nodes() <= session.nodes_per_submit());
    }
    acc = session.finish(acc);

    std::vector<float> values;
    const splat::Status downloaded = session.download(acc, &values);
    REQUIRE_MESSAGE(downloaded.ok(), downloaded.message());
    REQUIRE(values.size() == 64);
    for (float value : values) CHECK(std::fabs(value - 13.0f) < 1e-6f);

    // 12 additions over a 4-node budget cannot fit one submit.
    CHECK(session.submits() >= 2);
    CHECK(session.arena_used() > 0);
    CHECK(session.arena_used() <= session.arena_size());
    MESSAGE("submits=" << session.submits() << " arena=" << session.arena_used() << " bytes");
}

TEST_CASE("graph session carry copies across submit boundaries") {
    splat::Result<GgmlBackend> backend = GgmlBackend::create(/*prefer_vulkan=*/false);
    REQUIRE_MESSAGE(backend.ok(), backend.message());

    GgmlWeights none;
    GraphSession session(backend.value(), none, /*nodes_per_submit=*/1024, /*arena_bytes=*/16u << 20);

    std::vector<float> ramp(36);
    for (size_t index = 0; index < ramp.size(); ++index) ramp[index] = static_cast<float>(index);
    ggml_tensor* input = session.persistent("ramp", {6, 6});
    REQUIRE(session.ctx() != nullptr);
    REQUIRE_MESSAGE(none.upload(input, ramp.data(), ramp.size() * sizeof(float)).ok(), "upload");

    // x * 2 + 1, force a submit after each step.
    ggml_tensor* value = input;
    for (int step = 0; step < 3; ++step) {
        value = ggml_scale(session.ctx(), value, 2.0f);
        value = session.carry(value);
        value = ggml_add(session.ctx(), value, input);
        value = session.carry(value);
    }
    value = session.finish(value);

    std::vector<float> values;
    REQUIRE_MESSAGE(session.download(value, &values).ok(), "download");
    REQUIRE(values.size() == ramp.size());
    double expected = 0.0;
    for (size_t index = 0; index < ramp.size(); ++index) {
        expected = ramp[index];
        for (int step = 0; step < 3; ++step) expected = expected * 2.0 + ramp[index];
        CHECK(std::fabs(values[index] - expected) < 1e-4);
    }
    CHECK(session.submits() >= 6);
}
