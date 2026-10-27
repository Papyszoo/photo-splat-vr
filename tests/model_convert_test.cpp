#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

extern "C" {
#include "miniz/miniz.h"
#include "miniz/miniz_zip.h"
}

#include "ggml.h"
#include "gguf.h"

#include "common/half.h"
#include "model/checkpoint.h"
#include "model/converter.h"
#include "model/sha256.h"
#include "test_util.h"

namespace {

using splat::ConvertOptions;
using splat::ConvertStats;
using splat::Path;
using splat::Progress;
using splat::Status;

// ---- paths ------------------------------------------------------------------
Path test_dir() { return Path(__FILE__).parent_path(); }

Path fixture_path(const std::string& name) {
    return test_dir() / "fixtures" / "model" / name;
}

Path repo_root() { return test_dir().parent_path(); }

std::string env_or_empty(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

// The official checkpoint is 2.6 GB and never lives in the repo (spec rule 1),
// so the real-checkpoint case explains itself and returns when it is absent.
Path real_checkpoint_path() {
    const std::string from_env = env_or_empty("SPLAT_SHARP_CHECKPOINT");
    if (!from_env.empty()) return Path(from_env);
    return repo_root() / ".." / "weights" / splat::kSharpCheckpointName;
}

// The manifest is dumped by tools/convert/dump_manifest.py into runs/, next to
// weights/.
Path real_manifest_path() {
    const std::string from_env = env_or_empty("SPLAT_SHARP_MANIFEST");
    if (!from_env.empty()) return Path(from_env);
    return repo_root() / ".." / "runs" / "sharp-manifest.json";
}

// Converted output stays beside the checkpoint, under weights/.
Path real_gguf_path() { return real_checkpoint_path().parent_path() / "sharp_2572gikvuh-f16-v1.gguf"; }

// ---- manifest ---------------------------------------------------------------
struct ManifestTensor {
    std::string name;
    std::string gguf_name;
    std::string dtype;  // "float16" or "float32"
    std::vector<int64_t> shape;
    std::vector<uint64_t> indices;
    std::vector<float> values;

    uint64_t numel() const {
        uint64_t count = 1;
        for (int64_t dim : shape) count *= static_cast<uint64_t>(dim);
        return count;
    }
};

struct Manifest {
    std::string source;
    std::string sha256;
    uint64_t bytes = 0;
    uint64_t tensor_count = 0;
    uint64_t converted_count = 0;
    uint64_t dropped_count = 0;
    std::vector<std::string> dropped;
    std::vector<std::string> order;  // every state-dict key, in pickle order
    std::vector<ManifestTensor> tensors;
};

Manifest load_manifest(const Path& path) {
    std::ifstream file(path);
    REQUIRE_MESSAGE(file.good(), "cannot open manifest " << path);
    nlohmann::json root;
    file >> root;

    Manifest manifest;
    manifest.source = root.at("source").get<std::string>();
    manifest.sha256 = root.at("checkpoint_sha256").get<std::string>();
    manifest.bytes = root.at("checkpoint_bytes").get<uint64_t>();
    manifest.tensor_count = root.at("tensor_count").get<uint64_t>();
    manifest.converted_count = root.at("converted_tensor_count").get<uint64_t>();
    manifest.dropped_count = root.at("dropped_count").get<uint64_t>();
    for (const auto& name : root.at("dropped")) manifest.dropped.push_back(name.get<std::string>());
    for (const auto& name : root.at("tensor_order")) manifest.order.push_back(name.get<std::string>());

    for (const auto& entry : root.at("tensors")) {
        ManifestTensor tensor;
        tensor.name = entry.at("name").get<std::string>();
        tensor.gguf_name = entry.at("gguf_name").get<std::string>();
        tensor.dtype = entry.at("dtype").get<std::string>();
        for (const auto& dim : entry.at("shape")) tensor.shape.push_back(dim.get<int64_t>());
        for (const auto& index : entry.at("spot_indices")) tensor.indices.push_back(index.get<uint64_t>());
        for (const auto& value : entry.at("spot_values")) tensor.values.push_back(value.get<float>());
        REQUIRE(tensor.indices.size() == tensor.values.size());
        manifest.tensors.push_back(std::move(tensor));
    }
    return manifest;
}

// ---- reading the GGUF back --------------------------------------------------
// gguf_init_from_file(no_alloc) parses the header and tensor table without
// loading 1.3 GB of payload; spot values are then read straight from the file.
class GgufFile {
public:
    ~GgufFile() { close(); }

    bool open(const Path& path) {
        close();
        gguf_init_params params{};
        params.no_alloc = true;
        params.ctx = &ggml_;
        gguf_ = gguf_init_from_file(path.string().c_str(), params);
        if (gguf_ == nullptr) return false;
        file_.open(path, std::ios::binary);
        if (!file_) return false;
        data_offset_ = gguf_get_data_offset(gguf_);
        alignment_ = gguf_get_alignment(gguf_);
        return true;
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

    gguf_context* raw() const { return gguf_; }
    uint64_t data_offset() const { return data_offset_; }
    uint64_t alignment() const { return alignment_; }
    int64_t tensor_count() const { return gguf_get_n_tensors(gguf_); }

    int64_t find_tensor(const std::string& name) const { return gguf_find_tensor(gguf_, name.c_str()); }

    std::vector<int64_t> ne(int64_t id) const {
        const int64_t* ne = gguf_get_tensor_ne(gguf_, id);
        return {ne[0], ne[1], ne[2], ne[3]};
    }

    float read_value(int64_t id, uint64_t linear_index) const {
        const ggml_type type = gguf_get_tensor_type(gguf_, id);
        const size_t itemsize = type == GGML_TYPE_F16 ? 2 : 4;
        const uint64_t offset = data_offset_ + gguf_get_tensor_offset(gguf_, id) +
                                linear_index * itemsize;
        uint8_t bytes[4] = {};
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        file_.read(reinterpret_cast<char*>(bytes), static_cast<std::streamsize>(itemsize));
        REQUIRE_MESSAGE(file_.good(), "short read at GGUF offset " << offset);
        if (type == GGML_TYPE_F16) {
            uint16_t half = 0;
            std::memcpy(&half, bytes, 2);
            return splat::half_to_float(half);
        }
        float value = 0.0f;
        std::memcpy(&value, bytes, 4);
        return value;
    }

    std::string string_key(const std::string& key) const {
        const int64_t id = gguf_find_key(gguf_, key.c_str());
        REQUIRE_MESSAGE(id >= 0, "missing GGUF key " << key);
        REQUIRE(gguf_get_kv_type(gguf_, id) == GGUF_TYPE_STRING);
        return gguf_get_val_str(gguf_, id);
    }

    uint64_t u32_key(const std::string& key) const {
        const int64_t id = gguf_find_key(gguf_, key.c_str());
        REQUIRE_MESSAGE(id >= 0, "missing GGUF key " << key);
        return gguf_get_val_u32(gguf_, id);
    }

    std::vector<std::string> string_array_key(const std::string& key) const {
        const int64_t id = gguf_find_key(gguf_, key.c_str());
        REQUIRE_MESSAGE(id >= 0, "missing GGUF key " << key);
        REQUIRE(gguf_get_arr_type(gguf_, id) == GGUF_TYPE_STRING);
        std::vector<std::string> values;
        for (size_t i = 0; i < gguf_get_arr_n(gguf_, id); ++i) {
            values.emplace_back(gguf_get_arr_str(gguf_, id, i));
        }
        return values;
    }

private:
    gguf_context* gguf_ = nullptr;
    ggml_context* ggml_ = nullptr;
    mutable std::ifstream file_;
    uint64_t data_offset_ = 0;
    uint64_t alignment_ = 0;
};

// ---- comparing a GGUF against a manifest ------------------------------------
// `source_name` and `source_sha256` override what the GGUF metadata should say
// about its input: the zip64 test converts a rewritten copy of the fixture.
void check_gguf_matches(const Path& gguf_path, const Manifest& manifest,
                        const std::string& source_name, const std::string& source_sha256) {
    GgufFile file;
    REQUIRE_MESSAGE(file.open(gguf_path), "cannot open GGUF " << gguf_path);

    REQUIRE(file.tensor_count() == static_cast<int64_t>(manifest.tensors.size()));
    CHECK(file.string_key("general.architecture") == "sharp");
    CHECK(file.string_key("sharp.converter") == "splat_convert");
    CHECK(file.u32_key("sharp.converter_version") == 1);
    CHECK(file.string_key("sharp.backend") == "sharp");
    CHECK(file.string_key("sharp.source_sha256") == source_sha256);
    CHECK(file.u32_key("sharp.source_tensor_count") == manifest.tensor_count);
    CHECK(file.u32_key("sharp.tensor_count") == manifest.converted_count);
    CHECK(file.u32_key("sharp.dropped_tensor_count") == manifest.dropped_count);
    CHECK(file.string_key("sharp.source_filename") == source_name);

    // The changes list is the "derivative, with changes disclosed" label.
    const std::vector<std::string> changes = file.string_array_key("sharp.changes");
    REQUIRE(changes.size() >= 3);
    const std::string joined = [&changes] {
        std::string text;
        for (const std::string& change : changes) text += change + "\n";
        return text;
    }();
    CHECK(joined.find("float32 to float16") != std::string::npos);
    CHECK(joined.find("layout") != std::string::npos);
    CHECK(joined.find("dropped") != std::string::npos);

    const std::vector<std::string> dropped = file.string_array_key("sharp.dropped_tensors");
    CHECK(dropped.size() == manifest.dropped_count);
    for (const std::string& name : manifest.dropped) {
        CHECK_MESSAGE(std::find(dropped.begin(), dropped.end(), name) != dropped.end(),
                      "dropped tensor not disclosed: " << name);
    }

    const std::vector<std::string> full_names = file.string_array_key("sharp.source_tensor_names");
    REQUIRE(full_names.size() == manifest.tensors.size());

    uint64_t expected_data = 0;
    for (size_t i = 0; i < manifest.tensors.size(); ++i) {
        const ManifestTensor& expected = manifest.tensors[i];
        INFO("tensor " << i << " " << expected.gguf_name);
        CHECK(full_names[i] == expected.name);
        CHECK(gguf_get_tensor_name(file.raw(), static_cast<int64_t>(i)) == expected.gguf_name);

        const int64_t id = file.find_tensor(expected.gguf_name);
        REQUIRE_MESSAGE(id >= 0, "missing tensor " << expected.gguf_name);
        // The dropped tensors must not be there under their torch names either.
        CHECK(file.find_tensor(expected.name) == (expected.name == expected.gguf_name ? id : -1));
        CHECK(gguf_get_tensor_name(file.raw(), id) == expected.gguf_name);

        const ggml_type type = gguf_get_tensor_type(file.raw(), id);
        const bool fp16 = expected.dtype == "float16";
        CHECK(type == (fp16 ? GGML_TYPE_F16 : GGML_TYPE_F32));

        // Torch shape is outermost-first, ggml ne is reversed.
        const std::vector<int64_t> ne = file.ne(id);
        REQUIRE(ne.size() == 4);
        for (size_t d = 0; d < 4; ++d) {
            const int64_t want = d < expected.shape.size() ? expected.shape[expected.shape.size() - 1 - d] : 1;
            CHECK(ne[d] == want);
        }

        const uint64_t itemsize = fp16 ? 2 : 4;
        CHECK(gguf_get_tensor_size(file.raw(), id) == expected.numel() * itemsize);
        expected_data += (expected.numel() * itemsize + file.alignment() - 1) / file.alignment() *
                         file.alignment();

        for (size_t s = 0; s < expected.indices.size(); ++s) {
            const float actual = file.read_value(id, expected.indices[s]);
            const float want = expected.values[s];
            if (!fp16) {
                CHECK_MESSAGE(actual == want, "spot " << expected.indices[s] << " of "
                                                      << expected.gguf_name << ": " << actual
                                                      << " vs " << want);
            } else {
                const float scale = std::max(1.0f, std::max(std::abs(actual), std::abs(want)));
                CHECK_MESSAGE(std::abs(actual - want) <= 2e-3f * scale,
                              "spot " << expected.indices[s] << " of " << expected.gguf_name << ": "
                                      << actual << " vs " << want);
            }
        }
    }

    // Every declared byte must be present, padded to the alignment.
    CHECK(std::filesystem::file_size(gguf_path) == file.data_offset() + expected_data);
}

// ---- handcrafted pickles ----------------------------------------------------
// PROTO 2, then whatever the case appends. U+0000 is not special to the parser.
std::vector<uint8_t> pickle_with_opcode(uint8_t opcode) {
    std::vector<uint8_t> bytes = {0x80, 0x02};  // PROTO 2
    bytes.push_back(opcode);
    if (opcode == 'G') {  // BINFLOAT: eight big-endian bytes
        for (int i = 0; i < 8; ++i) bytes.push_back(0);
    }
    bytes.push_back('.');
    return bytes;
}

std::vector<uint8_t> pickle_with_global(const std::string& module, const std::string& name) {
    std::vector<uint8_t> bytes = {0x80, 0x02, 'c'};
    bytes.insert(bytes.end(), module.begin(), module.end());
    bytes.push_back('\n');
    bytes.insert(bytes.end(), name.begin(), name.end());
    bytes.push_back('\n');
    bytes.push_back(')');  // EMPTY_TUPLE
    bytes.push_back('R');  // REDUCE
    bytes.push_back('.');  // STOP
    return bytes;
}

// ---- crafted torch.save files ----------------------------------------------
// A malicious .pt must fail cleanly: the unpickler is a whitelist interpreter,
// but the element-count arithmetic around it still has to be overflow-checked.
void put_binunicode(std::vector<uint8_t>& bytes, const std::string& text) {
    bytes.push_back('X');  // BINUNICODE, 4-byte little-endian length
    const uint32_t length = static_cast<uint32_t>(text.size());
    for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>((length >> (8 * i)) & 0xffu));
    bytes.insert(bytes.end(), text.begin(), text.end());
}

void put_binint(std::vector<uint8_t>& bytes, int32_t value) {
    bytes.push_back('J');  // BININT, 4-byte little-endian, two's complement
    const uint32_t raw = static_cast<uint32_t>(value);
    for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>((raw >> (8 * i)) & 0xffu));
}

void put_global(std::vector<uint8_t>& bytes, const std::string& module,
                const std::string& name) {
    bytes.push_back('c');
    bytes.insert(bytes.end(), module.begin(), module.end());
    bytes.push_back('\n');
    bytes.insert(bytes.end(), name.begin(), name.end());
    bytes.push_back('\n');
}

// One `_rebuild_tensor_v2` entry in an otherwise empty OrderedDict.
std::vector<uint8_t> crafted_state_dict(const std::string& name, const std::string& storage_key,
                                        int32_t storage_numel, int32_t storage_offset,
                                        const std::vector<int32_t>& shape) {
    std::vector<uint8_t> bytes = {0x80, 0x02};  // PROTO 2
    put_global(bytes, "collections", "OrderedDict");
    bytes.push_back(')');  // EMPTY_TUPLE
    bytes.push_back('R');  // REDUCE
    bytes.push_back('(');  // MARK for SETITEMS

    put_binunicode(bytes, name);
    put_global(bytes, "torch._utils", "_rebuild_tensor_v2");
    bytes.push_back('(');  // MARK: the six REDUCE arguments
    bytes.push_back('(');  // MARK: the BINPERSID tuple
    put_binunicode(bytes, "storage");
    put_global(bytes, "torch", "FloatStorage");
    put_binunicode(bytes, storage_key);
    put_binunicode(bytes, "cpu");
    put_binint(bytes, storage_numel);
    bytes.push_back('t');  // TUPLE
    bytes.push_back('Q');  // BINPERSID

    put_binint(bytes, storage_offset);
    bytes.push_back('(');  // MARK: size
    for (int32_t dim : shape) put_binint(bytes, dim);
    bytes.push_back('t');  // TUPLE
    bytes.push_back('(');  // MARK: stride, row major
    for (size_t i = 0; i < shape.size(); ++i) {
        int64_t stride = 1;
        for (size_t j = i + 1; j < shape.size(); ++j) stride *= shape[j];
        put_binint(bytes, static_cast<int32_t>(stride));
    }
    bytes.push_back('t');  // TUPLE
    bytes.push_back('\x89');  // NEWFALSE: requires_grad
    bytes.push_back('}');      // EMPTY_DICT: backward hooks
    bytes.push_back('t');  // TUPLE: the REDUCE arguments
    bytes.push_back('R');  // REDUCE
    bytes.push_back('u');  // SETITEMS on the OrderedDict
    bytes.push_back('.');  // STOP
    return bytes;
}

// Writes a torch.save-style zip with one storage entry of `storage_bytes`.
Status write_crafted_checkpoint(const Path& path, const std::vector<uint8_t>& pickle,
                               size_t storage_bytes) {
    std::vector<uint8_t> storage(storage_bytes, 0);
    const std::string byteorder = "little";
    struct Entry {
        const char* name;
        const void* data;
        size_t size;
    };
    const Entry entries[3] = {{"crafted/data.pkl", pickle.data(), pickle.size()},
                              {"crafted/byteorder", byteorder.data(), byteorder.size()},
                              {"crafted/data/0", storage.data(), storage.size()}};
    mz_zip_archive archive{};
    std::memset(&archive, 0, sizeof(archive));
    if (!mz_zip_writer_init_file(&archive, path.string().c_str(), 0)) {
        return Status::error("cannot create " + path.string());
    }
    for (const Entry& entry : entries) {
        if (!mz_zip_writer_add_mem(&archive, entry.name, entry.data, entry.size,
                                   MZ_NO_COMPRESSION)) {
            mz_zip_writer_end(&archive);
            return Status::error("cannot add " + std::string(entry.name));
        }
    }
    if (!mz_zip_writer_finalize_archive(&archive)) {
        mz_zip_writer_end(&archive);
        return Status::error("cannot finalise " + path.string());
    }
    mz_zip_writer_end(&archive);
    return Status::success();
}

// ---- zip64 rewrite using the vendored miniz writer --------------------------
Status write_zip64(const Path& source, const Path& destination) {    splat::ZipArchive reader;
    Status status = reader.open(source);
    if (!status.ok()) return status;

    mz_zip_archive archive{};
    std::memset(&archive, 0, sizeof(archive));
    if (!mz_zip_writer_init_file_v2(&archive, destination.string().c_str(), 0, MZ_ZIP_FLAG_WRITE_ZIP64)) {
        return Status::error("cannot create zip64 fixture");
    }

    for (const splat::ZipEntryInfo& entry : reader.entries()) {
        const auto bytes = reader.read_entry(entry.name, 64u << 20);
        if (!bytes.ok()) {
            mz_zip_writer_end(&archive);
            return Status::error(bytes.message());
        }
        if (!mz_zip_writer_add_mem(&archive, entry.name.c_str(), bytes.value().data(),
                                   bytes.value().size(), MZ_NO_COMPRESSION)) {
            mz_zip_writer_end(&archive);
            return Status::error("cannot add " + entry.name + " to the zip64 fixture");
        }
    }
    if (!mz_zip_writer_finalize_archive(&archive)) {
        mz_zip_writer_end(&archive);
        return Status::error("cannot finalise the zip64 fixture");
    }
    mz_zip_writer_end(&archive);
    return Status::success();
}

}  // namespace

TEST_CASE("synthetic checkpoint: the pickle tensor table") {
    const Path checkpoint = fixture_path("synthetic.pt");
    REQUIRE_MESSAGE(std::filesystem::exists(checkpoint), "missing fixture " << checkpoint);
    const Manifest manifest = load_manifest(fixture_path("synthetic.json"));

    const auto opened = splat::open_checkpoint(checkpoint);
    REQUIRE_MESSAGE(opened.ok(), opened.message());
    const splat::Checkpoint& state = opened.value();

    // torch.save (allowZip64) always writes the zip64 end-of-central-directory
    // record, so even the 9 KB fixture is a zip64 archive.
    CHECK(state.zip64);
    CHECK(state.byteorder == "little");
    CHECK(state.prefix == "synthetic/");
    REQUIRE(state.tensors.size() == manifest.tensor_count);

    std::vector<std::string> seen_keys;
    REQUIRE(manifest.order.size() == manifest.tensor_count);
    for (size_t i = 0; i < manifest.tensor_count; ++i) {
        const splat::CheckpointTensor& tensor = state.tensors[i];
        INFO("tensor " << i << " " << tensor.record.name);
        CHECK(tensor.record.name == manifest.order[i]);
        CHECK(tensor.record.dtype == splat::StorageDtype::Float32);
        CHECK(tensor.record.storage_offset == 0);
        CHECK(tensor.record.contiguous());
        CHECK(std::find(seen_keys.begin(), seen_keys.end(), tensor.record.storage_key) == seen_keys.end());
        seen_keys.push_back(tensor.record.storage_key);
        CHECK(tensor.zip_name == state.prefix + "data/" + tensor.record.storage_key);
    }

    // The first record is the first tensor written by tests/fixtures/model/make_fixture.py.
    CHECK(state.tensors.front().record.name == "feature_model.decoder.convs.0.weight");
    CHECK(state.tensors.front().record.shape == std::vector<int64_t>{8, 16, 1, 1});
    CHECK(state.tensors.front().record.numel() == 128);
    CHECK(state.tensors.back().record.name ==
          "depth_alignment.scale_map_estimator.conv_out.bias");
}

TEST_CASE("synthetic checkpoint converts to a GGUF that reads back") {
    const Path checkpoint = fixture_path("synthetic.pt");
    const Manifest manifest = load_manifest(fixture_path("synthetic.json"));
    splat_test::TempDir temp("convert-fixture");
    const Path output = temp.path() / "synthetic.gguf";

    ConvertOptions options;
    options.verify_sha256 = false;  // the fixture is not the pinned checkpoint
    Progress progress;
    const auto result = splat::convert_checkpoint(checkpoint, output, options, progress);
    REQUIRE_MESSAGE(result.ok(), result.message());
    const ConvertStats& stats = result.value();

    CHECK(stats.tensor_count == manifest.converted_count);
    CHECK(stats.dropped_count == manifest.dropped_count);
    CHECK(stats.source_bytes == manifest.bytes);
    CHECK(stats.source_sha256 == manifest.sha256);
    CHECK(stats.zip64);
    CHECK(stats.data_offset > 0);
    CHECK(stats.gguf_bytes == std::filesystem::file_size(output));
    CHECK(stats.seconds >= 0.0);
    CHECK(progress.fraction() > 0.99f);

    check_gguf_matches(output, manifest, manifest.source, manifest.sha256);

    // The dropped tensors are named in metadata, and no tensor carries a name
    // that starts with the dead subtree.
    GgufFile file;
    REQUIRE(file.open(output));
    for (int64_t id = 0; id < file.tensor_count(); ++id) {
        const std::string name = gguf_get_tensor_name(file.raw(), id);
        CHECK(name.rfind("depth_alignment.", 0) != 0);
        CHECK(name.find(".head.weight") == std::string::npos);
        CHECK(name.size() < 64);
    }
}

TEST_CASE("conversion refuses a checkpoint whose SHA-256 is not pinned") {
    splat_test::TempDir temp("convert-hash");
    const Path output = temp.path() / "refused.gguf";

    ConvertOptions options;  // verify_sha256 defaults to true
    Progress progress;
    const auto result = splat::convert_checkpoint(fixture_path("synthetic.pt"), output, options, progress);
    REQUIRE_FALSE(result.ok());
    CHECK(result.message().find("does not match the pinned") != std::string::npos);
    CHECK(result.message().find(splat::kSharpCheckpointSha256) != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(output));
}

TEST_CASE("dead tensors are the ViT heads and the depth-adjustment U-Net") {
    CHECK(splat::is_dead_tensor("depth_alignment.scale_map_estimator.conv_out.weight"));
    CHECK(splat::is_dead_tensor(
        "monodepth_model.monodepth_predictor.encoder.patch_encoder.head.weight"));
    CHECK(splat::is_dead_tensor(
        "monodepth_model.monodepth_predictor.encoder.image_encoder.head.bias"));
    CHECK_FALSE(splat::is_dead_tensor(
        "monodepth_model.monodepth_predictor.encoder.patch_encoder.blocks.0.attn.proj.weight"));
    CHECK_FALSE(splat::is_dead_tensor("feature_model.decoder.convs.0.weight"));
    CHECK_FALSE(splat::is_dead_tensor("prediction_head.weight"));

    // ggml caps tensor names at 63 bytes; the torch prefix is stripped so the
    // real checkpoint's 84-character names fit.
    CHECK(splat::gguf_tensor_name(
              "monodepth_model.monodepth_predictor.encoder.patch_encoder.blocks.23.attn.proj.weight") ==
          "encoder.patch_encoder.blocks.23.attn.proj.weight");
    CHECK(splat::gguf_tensor_name("feature_model.decoder.convs.0.weight") ==
          "feature_model.decoder.convs.0.weight");
}

TEST_CASE("the unpickler rejects unknown opcodes") {
    const auto binary_float = pickle_with_opcode('G');
    const auto result = splat::unpickle_state_dict(binary_float, "handcrafted");
    REQUIRE_FALSE(result.ok());
    CHECK(result.message().find("unsupported pickle opcode 0x47") != std::string::npos);
    CHECK(result.message().find("BINFLOAT") != std::string::npos);
    CHECK(result.message().find("handcrafted") != std::string::npos);
}

TEST_CASE("the unpickler rejects unknown globals") {
    // os.system() is the classic pickle exploit; the parser must never reach it.
    const auto result = splat::unpickle_state_dict(pickle_with_global("os", "system"), "handcrafted");
    REQUIRE_FALSE(result.ok());
    CHECK(result.message().find("unsupported global 'os.system'") != std::string::npos);
    CHECK(result.message().find("torch._utils._rebuild_tensor_v2") != std::string::npos);

    const auto other = splat::unpickle_state_dict(pickle_with_global("builtins", "eval"), "handcrafted");
    REQUIRE_FALSE(other.ok());
    CHECK(other.message().find("builtins.eval") != std::string::npos);
}

TEST_CASE("the unpickler rejects a truncated stream") {
    std::vector<uint8_t> truncated = {0x80, 0x02, 'X', 0x40, 0x00};  // BINUNICODE claiming 16 KiB
    const auto result = splat::unpickle_state_dict(truncated, "handcrafted");
    REQUIRE_FALSE(result.ok());
    CHECK(result.message().find("truncated pickle") != std::string::npos);
}

TEST_CASE("a zip64 checkpoint is read and converted") {
    splat_test::TempDir temp("convert-zip64");
    const Path zip64 = temp.path() / "synthetic-zip64.pt";
    const Status written = write_zip64(fixture_path("synthetic.pt"), zip64);
    REQUIRE_MESSAGE(written.ok(), written.message());

    const Manifest manifest = load_manifest(fixture_path("synthetic.json"));
    const auto opened = splat::open_checkpoint(zip64);
    REQUIRE_MESSAGE(opened.ok(), opened.message());
    CHECK(opened.value().zip64);
    CHECK(opened.value().tensors.size() == manifest.tensor_count);

    ConvertOptions options;
    options.verify_sha256 = false;
    Progress progress;
    const Path output = temp.path() / "synthetic-zip64.gguf";
    const auto result = splat::convert_checkpoint(zip64, output, options, progress);
    REQUIRE_MESSAGE(result.ok(), result.message());
    CHECK(result.value().zip64);
    CHECK(result.value().tensor_count == manifest.converted_count);

    const auto zip64_digest = splat::sha256_file(zip64);
    REQUIRE_MESSAGE(zip64_digest.ok(), zip64_digest.message());
    check_gguf_matches(output, manifest, zip64.filename().string(), zip64_digest.value());
}


TEST_CASE("a crafted checkpoint cannot overflow the element arithmetic") {
    // Dimensions near INT32_MAX: the product is 2^93 elements. numel() must
    // report the overflow instead of wrapping to a small, plausible-looking
    // count, so no bound check downstream can be fooled.
    splat::TensorRecord huge;
    huge.shape = {2147483647, 2147483647, 2147483647};
    huge.stride = {1, 1, 1};
    CHECK(huge.numel() == splat::TensorRecord::kInvalidNumel);
    CHECK_FALSE(huge.contiguous());

    splat::TensorRecord negative;
    negative.shape = {-4};
    negative.stride = {1};
    CHECK(negative.numel() == splat::TensorRecord::kInvalidNumel);

    splat::TensorRecord fine;
    fine.shape = {8, 16, 1, 1};
    fine.stride = {16, 1, 1, 1};
    CHECK(fine.numel() == 128u);
    CHECK(fine.contiguous());
}

TEST_CASE("a crafted checkpoint with an overflowing shape is rejected") {
    splat_test::TempDir temp("crafted-overflow");
    const auto path = temp.path() / "overflow.pt";
    const auto pickle = crafted_state_dict("weight", "0", 1, 0, {2147483647, 2147483647, 2147483647});
    REQUIRE_MESSAGE(write_crafted_checkpoint(path, pickle, 4).ok(), "write");
    const auto checkpoint = splat::open_checkpoint(path);
    REQUIRE_FALSE(checkpoint.ok());
    CHECK_MESSAGE(checkpoint.message().find("overflows") != std::string::npos,
                  checkpoint.message());
}

TEST_CASE("a crafted checkpoint with a negative offset is rejected") {
    splat_test::TempDir temp("crafted-offset");
    const auto path = temp.path() / "offset.pt";
    const auto pickle = crafted_state_dict("weight", "0", 1, -1, {1});
    REQUIRE_MESSAGE(write_crafted_checkpoint(path, pickle, 4).ok(), "write");
    const auto checkpoint = splat::open_checkpoint(path);
    REQUIRE_FALSE(checkpoint.ok());
    CHECK_MESSAGE(checkpoint.message().find("negative storage offset") != std::string::npos,
                  checkpoint.message());
}

TEST_CASE("a crafted checkpoint with a negative storage size is rejected") {
    splat_test::TempDir temp("crafted-storage");
    const auto path = temp.path() / "storage.pt";
    const auto pickle = crafted_state_dict("weight", "0", -1, 0, {1});
    REQUIRE_MESSAGE(write_crafted_checkpoint(path, pickle, 4).ok(), "write");
    const auto checkpoint = splat::open_checkpoint(path);
    REQUIRE_FALSE(checkpoint.ok());
    CHECK_MESSAGE(checkpoint.message().find("negative storage element count") != std::string::npos,
                  checkpoint.message());
}

TEST_CASE("a crafted checkpoint whose storage is too small is rejected") {
    splat_test::TempDir temp("crafted-small");
    const auto path = temp.path() / "small.pt";
    // Four elements declared, one float present: the tensor would read past the
    // storage, which must fail instead of truncating silently.
    const auto pickle = crafted_state_dict("weight", "0", 4, 0, {4});
    REQUIRE_MESSAGE(write_crafted_checkpoint(path, pickle, 4).ok(), "write");
    const auto checkpoint = splat::open_checkpoint(path);
    REQUIRE_FALSE(checkpoint.ok());
    CHECK_MESSAGE(checkpoint.message().find("bytes") != std::string::npos, checkpoint.message());
}

TEST_CASE("a valid crafted checkpoint still opens") {
    // Sanity check on the crafted-file helper itself: without the hostile
    // numbers it parses.
    splat_test::TempDir temp("crafted-ok");
    const auto path = temp.path() / "ok.pt";
    const auto pickle = crafted_state_dict("weight", "0", 4, 0, {2, 2});
    REQUIRE_MESSAGE(write_crafted_checkpoint(path, pickle, 16).ok(), "write");
    const auto checkpoint = splat::open_checkpoint(path);
    REQUIRE_MESSAGE(checkpoint.ok(), checkpoint.message());
    REQUIRE(checkpoint.value().tensors.size() == 1);
    CHECK(checkpoint.value().tensors[0].record.name == "weight");
    CHECK(checkpoint.value().tensors[0].record.numel() == 4u);
}

TEST_CASE("the real SHARP checkpoint converts and matches the PyTorch manifest") {
    const Path checkpoint = real_checkpoint_path();
    if (!std::filesystem::exists(checkpoint)) {
        MESSAGE("skipping: no checkpoint at " << checkpoint
                << " (set SPLAT_SHARP_CHECKPOINT to run this case)");
        return;
    }
    const Path manifest_path = real_manifest_path();
    if (!std::filesystem::exists(manifest_path)) {
        MESSAGE("skipping: no manifest at " << manifest_path
                << " (run tools/convert/dump_manifest.py)");
        return;
    }
    const Manifest manifest = load_manifest(manifest_path);
    CHECK(manifest.tensor_count == splat::kSharpCheckpointTensorCount);

    // Rule 2: the pinned hash is verified before any conversion.
    const auto digest = splat::sha256_file(checkpoint);
    REQUIRE_MESSAGE(digest.ok(), digest.message());
    CHECK(digest.value() == splat::kSharpCheckpointSha256);
    CHECK(std::filesystem::file_size(checkpoint) == splat::kSharpCheckpointBytes);

    const auto opened = splat::open_checkpoint(checkpoint);
    REQUIRE_MESSAGE(opened.ok(), opened.message());
    // torch.save writes the zip64 end record even though every entry is under
    // 4 GiB: the 2.6 GB checkpoint is read through miniz's zip64 path.
    CHECK(opened.value().zip64);
    CHECK(opened.value().tensors.size() == manifest.tensor_count);

    // Convert once into weights/ and reuse it on later runs: the file is 1.3 GB
    // and a rebuild should not pay for it again.
    const Path gguf = real_gguf_path();
    ConvertOptions options;
    options.verify_sha256 = false;  // just verified above, do not read 2.6 GB twice
    if (!std::filesystem::exists(gguf)) {
        Progress progress;
        const auto converted = splat::convert_checkpoint(checkpoint, gguf, options, progress);
        REQUIRE_MESSAGE(converted.ok(), converted.message());
        CHECK(converted.value().tensor_count == manifest.converted_count);
        CHECK(converted.value().dropped_count == manifest.dropped_count);
    }

    check_gguf_matches(gguf, manifest, manifest.source, splat::kSharpCheckpointSha256);
}
