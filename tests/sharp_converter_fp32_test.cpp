// The SHARP profile of the converter ignored `--fp32`: `ConvertOptions::keep_fp32`
// was only honoured by the DA2 safetensors profile, so the flag silently left
// every rank >= 2 tensor in fp16 and the fp32 parity GGUF could not be produced
// by `splat_convert` at all. This case is the regression test for the fix:
//
//   - default: rank >= 2 -> F16, 1-D -> F32 (unchanged)
//   - --fp32:  every tensor F32, and the metadata says so
//
// It runs on the synthetic checkpoint in tests/fixtures/model, so it needs no
// data outside the repo.

#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "ggml.h"
#include "gguf.h"

#include <nlohmann/json.hpp>

#include "common/half.h"
#include "model/converter.h"
#include "test_util.h"

namespace {

using splat::ConvertOptions;
using splat::Path;
using splat::Progress;

Path fixture_path(const std::string& name) {
    return Path(__FILE__).parent_path() / "fixtures" / "model" / name;
}

// Minimal reader: the tensor table with types, and one float value.
class GgufFile {
public:
    ~GgufFile() { close(); }

    bool open(const Path& path) {
        close();
        ggml_init_params ggml_params{};
        ggml_params.no_alloc = true;
        gguf_init_params params{};
        params.no_alloc = true;
        params.ctx = &ggml_;
        gguf_ = gguf_init_from_file(path.string().c_str(), params);
        if (gguf_ == nullptr) return false;
        file_.open(path, std::ios::binary);
        data_offset_ = gguf_get_data_offset(gguf_);
        return static_cast<bool>(file_);
    }

    void close() {
        if (gguf_ != nullptr) {
            gguf_free(gguf_);
            gguf_ = nullptr;
        }
        if (ggml_ != nullptr) {
            ggml_free(ggml_);
            ggml_ = nullptr;
        }
    }

    int64_t count() const { return gguf_get_n_tensors(gguf_); }
    std::string name(int64_t index) const { return gguf_get_tensor_name(gguf_, index); }
    ggml_type type(int64_t index) const { return gguf_get_tensor_type(gguf_, index); }
    const int64_t* ne(int64_t index) const { return gguf_get_tensor_ne(gguf_, index); }

    float value(int64_t index, uint64_t linear) const {
        const ggml_type kind = type(index);
        const size_t itemsize = kind == GGML_TYPE_F16 ? 2 : 4;
        const uint64_t offset =
            data_offset_ + gguf_get_tensor_offset(gguf_, index) + linear * itemsize;
        uint8_t bytes[4] = {};
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        file_.read(reinterpret_cast<char*>(bytes), static_cast<std::streamsize>(itemsize));
        REQUIRE(static_cast<bool>(file_));
        if (kind == GGML_TYPE_F16) {
            uint16_t half = 0;
            std::memcpy(&half, bytes, 2);
            return splat::half_to_float(half);
        }
        float result = 0.0f;
        std::memcpy(&result, bytes, 4);
        return result;
    }

    std::vector<std::string> string_array(const std::string& key) const {
        const int64_t id = gguf_find_key(gguf_, key.c_str());
        REQUIRE(id >= 0);
        std::vector<std::string> values;
        for (size_t index = 0; index < gguf_get_arr_n(gguf_, id); ++index) {
            values.emplace_back(gguf_get_arr_str(gguf_, id, index));
        }
        return values;
    }

private:
    gguf_context* gguf_ = nullptr;
    ggml_context* ggml_ = nullptr;
    mutable std::ifstream file_;
    uint64_t data_offset_ = 0;
};

}  // namespace

TEST_CASE("the SHARP profile honours --fp32") {
    splat_test::TempDir temp("convert-fp32");
    const Path checkpoint = fixture_path("synthetic.pt");
    REQUIRE_MESSAGE(std::filesystem::exists(checkpoint), "missing fixture " << checkpoint);

    ConvertOptions defaults;
    defaults.verify_sha256 = false;
    ConvertOptions fp32 = defaults;
    fp32.keep_fp32 = true;

    Progress progress;
    const Path fp16_path = temp.path() / "synthetic-fp16.gguf";
    const Path fp32_path = temp.path() / "synthetic-fp32.gguf";
    const auto fp16_stats = splat::convert_checkpoint(checkpoint, fp16_path, defaults, progress);
    REQUIRE_MESSAGE(fp16_stats.ok(), fp16_stats.message());
    const auto fp32_stats = splat::convert_checkpoint(checkpoint, fp32_path, fp32, progress);
    REQUIRE_MESSAGE(fp32_stats.ok(), fp32_stats.message());
    CHECK(fp32_stats.value().tensor_count == fp16_stats.value().tensor_count);

    GgufFile fp16_file;
    GgufFile fp32_file;
    REQUIRE_MESSAGE(fp16_file.open(fp16_path), "cannot open " << fp16_path);
    REQUIRE_MESSAGE(fp32_file.open(fp32_path), "cannot open " << fp32_path);
    REQUIRE(fp16_file.count() == fp32_file.count());

    // The manifest is the fixture's own description of what dtype each tensor
    // should have (tools/convert/dump_manifest.py wrote it).
    nlohmann::json manifest;
    {
        std::ifstream stream(fixture_path("synthetic.json"));
        REQUIRE(static_cast<bool>(stream));
        stream >> manifest;
    }
    size_t rank_two = 0;
    for (const auto& entry : manifest.at("tensors")) {
        const std::string name = entry.at("gguf_name").get<std::string>();
        const bool want_fp16 = entry.at("dtype").get<std::string>() == "float16";
        int64_t index = -1;
        for (int64_t candidate = 0; candidate < fp32_file.count(); ++candidate) {
            if (fp32_file.name(candidate) == name) index = candidate;
        }
        REQUIRE_MESSAGE(index >= 0, "missing tensor " << name);
        CHECK(entry.at("shape").size() >= 1);
        if (want_fp16) ++rank_two;
        // --fp32 keeps everything float32, 1-D tensors included.
        CHECK_MESSAGE(fp32_file.type(index) == GGML_TYPE_F32,
                      "--fp32 left " << name << " as " << ggml_type_name(fp32_file.type(index)));
        // ... and the default path still applies the fp16 rule.
        CHECK_MESSAGE(fp16_file.type(index) == (want_fp16 ? GGML_TYPE_F16 : GGML_TYPE_F32),
                      "default dtype changed for " << name);
        if (!want_fp16) continue;
        // The fp32 file carries the checkpoint's exact values; the fp16 one is a
        // quantised copy of the same tensor.
        const auto& indices = entry.at("spot_indices");
        const auto& values = entry.at("spot_values");
        for (size_t spot = 0; spot < indices.size(); ++spot) {
            const uint64_t linear = indices.at(spot).get<uint64_t>();
            const float exact = fp32_file.value(index, linear);
            const float expected = values.at(spot).get<float>();
            CHECK_MESSAGE(exact == expected, name << " spot " << linear << ": " << exact
                                                  << " vs " << expected);
            const float quantised = fp16_file.value(index, linear);
            CHECK_MESSAGE(std::abs(quantised - expected) <=
                              2e-3f * std::max(1.0f, std::abs(expected)),
                          name << " fp16 spot " << linear << ": " << quantised << " vs "
                               << expected);
        }
    }
    // The fixture must actually exercise the fp16 rule.
    CHECK(rank_two >= 4);

    // Disclosure: the metadata has to say the derivative is float32.
    const std::vector<std::string> changes = fp32_file.string_array("sharp.changes");
    bool says_fp32 = false;
    for (const std::string& change : changes) {
        if (change.find("float32") != std::string::npos && change.find("float16") == std::string::npos) {
            says_fp32 = true;
        }
    }
    CHECK_MESSAGE(says_fp32, "sharp.changes does not disclose the float32 dtype");
}
