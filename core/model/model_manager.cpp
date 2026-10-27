#include "model/model_manager.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <system_error>

#include <nlohmann/json.hpp>

#include "gguf.h"

#include "model/checkpoint.h"
#include "model/da2_profile.h"
#include "model/license.h"
#include "model/sha256.h"

namespace splat {
namespace {

constexpr uint32_t kStateFormatVersion = 1;
constexpr uint64_t kMaxStateBytes = 1u << 20;

Path record_path(const Path& state_dir, const std::string& backend) {
    return state_dir / "models" / (backend + ".json");
}

std::string part_suffix() { return ".part"; }

bool path_exists(const Path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
}

uint64_t size_or_zero(const Path& path) {
    std::error_code error;
    const uintmax_t size = std::filesystem::file_size(path, error);
    return error ? 0 : static_cast<uint64_t>(size);
}

void remove_if_present(const Path& path) {
    std::error_code error;
    std::filesystem::remove(path, error);
}

Status ensure_directory(const Path& path) {
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error) return Status::error("cannot create " + path.string() + ": " + error.message());
    return Status::success();
}

// Copies or moves `from` to `to`, creating the parent directory. A move across
// file systems falls back to copy + remove. The destination is replaced.
Status move_or_copy(const Path& from, const Path& to, bool move) {
    Status directory = ensure_directory(to.parent_path());
    if (!directory.ok()) return directory;

    if (move) {
        std::error_code error;
        std::filesystem::rename(from, to, error);
        if (!error) return Status::success();
    }

    const Path temporary = to.string() + ".tmp";
    std::error_code error;
    std::filesystem::copy_file(from, temporary, std::filesystem::copy_options::overwrite_existing,
                               error);
    if (error) {
        remove_if_present(temporary);
        return Status::error("cannot copy " + from.string() + " to " + to.string() + ": " +
                             error.message());
    }
    std::filesystem::rename(temporary, to, error);
    if (error) {
        remove_if_present(temporary);
        return Status::error("cannot replace " + to.string() + ": " + error.message());
    }
    if (move) {
        std::filesystem::remove(from, error);  // best effort: the copy is complete
    }
    return Status::success();
}

struct GgufHeader {
    int64_t tensor_count = 0;
    std::map<std::string, std::string> strings;
    std::map<std::string, uint64_t> integers;
};

// Header only: no tensor allocation, so checking a 1.2 GiB runtime file costs
// one small read.
Result<GgufHeader> read_gguf_header(const Path& path) {
    if (!path_exists(path)) return Result<GgufHeader>::failure("no such file: " + path.string());

    gguf_init_params params{};
    params.no_alloc = true;
    params.ctx = nullptr;
    gguf_context* gguf = gguf_init_from_file(path.string().c_str(), params);
    if (gguf == nullptr) {
        return Result<GgufHeader>::failure("cannot read the GGUF header of " + path.string());
    }

    GgufHeader header;
    header.tensor_count = gguf_get_n_tensors(gguf);
    const int64_t count = gguf_get_n_kv(gguf);
    for (int64_t index = 0; index < count; ++index) {
        const std::string key = gguf_get_key(gguf, index);
        switch (gguf_get_kv_type(gguf, index)) {
            case GGUF_TYPE_STRING:
                header.strings[key] = gguf_get_val_str(gguf, index);
                break;
            case GGUF_TYPE_UINT8:
                header.integers[key] = gguf_get_val_u8(gguf, index);
                break;
            case GGUF_TYPE_UINT16:
                header.integers[key] = gguf_get_val_u16(gguf, index);
                break;
            case GGUF_TYPE_UINT32:
                header.integers[key] = gguf_get_val_u32(gguf, index);
                break;
            case GGUF_TYPE_UINT64:
                header.integers[key] = gguf_get_val_u64(gguf, index);
                break;
            default:
                break;  // arrays and floats are not part of the identity check
        }
    }
    gguf_free(gguf);
    return Result<GgufHeader>::success(std::move(header));
}

bool read_text_file(const Path& path, std::string* text) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    *text = std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return static_cast<bool>(file) || file.eof();
}

}  // namespace

const char* model_state_name(ModelState state) {
    switch (state) {
        case ModelState::LicenseRequired: return "license_required";
        case ModelState::Accepted: return "accepted";
        case ModelState::Downloading: return "downloading";
        case ModelState::Verifying: return "verifying";
        case ModelState::Converting: return "converting";
        case ModelState::Ready: return "ready";
        case ModelState::Failed: return "failed";
    }
    return "failed";
}

bool model_state_from_name(const std::string& name, ModelState* state) {
    if (state == nullptr) return false;
    for (const ModelState candidate :
         {ModelState::LicenseRequired, ModelState::Accepted, ModelState::Downloading,
          ModelState::Verifying, ModelState::Converting, ModelState::Ready, ModelState::Failed}) {
        if (name == model_state_name(candidate)) {
            *state = candidate;
            return true;
        }
    }
    return false;
}

const ModelBackendSpec& sharp_model_spec() {
    static const ModelBackendSpec spec{
        "sharp",
        "SHARP",
        kSharpCheckpointName,
        "https://ml-site.cdn-apple.com/models/sharp/sharp_2572gikvuh.pt",
        kSharpCheckpointSha256,
        kSharpCheckpointBytes,
        "sharp",
        1,
        "https://raw.githubusercontent.com/apple-aiml-research/ml-sharp/main/LICENSE_MODEL",
        "LICENSE_MODEL (ml-sharp)",
    };
    return spec;
}

const ModelBackendSpec& da2_model_spec() {
    static const ModelBackendSpec spec{
        "da2",
        "Depth Anything V2 Small",
        "model.safetensors",  // official file name at kDepthAnythingV2SmallUrl
        kDepthAnythingV2SmallUrl,
        kDepthAnythingV2SmallSha256,
        kDepthAnythingV2SmallBytes,
        "depth-anything-v2",
        1,
        "",  // Apache-2.0: enabled by default, notice on About, no gate
        "Apache-2.0",
    };
    return spec;
}

ModelManager::ModelManager(ModelManagerOptions options)
    : state_dir_(std::move(options.state_dir)),
      fetcher_(options.fetcher),
      specs_(options.specs),
      converter_(std::move(options.converter)) {
    if (specs_.empty()) {
        specs_.push_back(sharp_model_spec());
        specs_.push_back(da2_model_spec());
    }
}

const ModelBackendSpec* ModelManager::backend(const std::string& name) const {
    for (const ModelBackendSpec& spec : specs_) {
        if (spec.name == name) return &spec;
    }
    return nullptr;
}

std::vector<std::string> ModelManager::backend_names() const {
    std::vector<std::string> names;
    names.reserve(specs_.size());
    for (const ModelBackendSpec& spec : specs_) names.push_back(spec.name);
    return names;
}

Path ModelManager::inbox_dir() const { return state_dir_ / "inbox"; }
Path ModelManager::downloads_dir() const { return state_dir_ / "downloads"; }
Path ModelManager::models_dir() const { return state_dir_ / "models"; }
Path ModelManager::gguf_path(const std::string& backend_name) const {
    return models_dir() / (backend_name + ".gguf");
}

bool ModelManager::license_accepted(const std::string& name) const {
    // A missing or unreadable licences file fails closed: nothing is accepted.
    // The gate compares the backend only; the text's hash is checked by
    // fetch_license_text()/accept_license() when the licence is shown again.
    const Result<LicenseState> state = read_license_state(state_dir_);
    if (!state.ok()) return false;
    for (const LicenseAcceptance& entry : state.value().accepted) {
        if (entry.backend == name) return true;
    }
    return false;
}

Result<std::string> ModelManager::fetch_license_text(const std::string& name) {
    const ModelBackendSpec* spec = backend(name);
    if (spec == nullptr) return Result<std::string>::failure("unknown backend '" + name + "'");
    if (spec->license_url.empty()) {
        return Result<std::string>::failure("backend '" + name + "' needs no licence acceptance (" +
                                            spec->license_name + ")");
    }
    if (fetcher_ == nullptr) {
        return Result<std::string>::failure("no HTTP fetcher to read " + spec->license_url);
    }
    return fetcher_->get_text(spec->license_url, 1u << 20);
}

Status ModelManager::accept_license(const std::string& name, const std::string& text) {
    const ModelBackendSpec* spec = backend(name);
    if (spec == nullptr) return Status::error("unknown backend '" + name + "'");
    if (spec->license_url.empty()) {
        return Status::error("backend '" + name + "' needs no licence acceptance (" +
                             spec->license_name + ")");
    }
    if (text.empty()) return Status::error("refusing to record an empty licence text");

    LicenseAcceptance acceptance;
    acceptance.backend = spec->name;
    acceptance.license_sha256 = Sha256::of_hex(text.data(), text.size());
    acceptance.accepted_at = current_timestamp_utc();
    return record_license_acceptance(state_dir_, acceptance);
}

Result<ModelRecord> ModelManager::load_record(const ModelBackendSpec& spec) {
    ModelRecord record;
    record.backend = spec.name;
    record.converter_version = spec.converter_version;
    record.state = spec.license_url.empty() || license_accepted(spec.name) ? ModelState::Accepted
                                                                          : ModelState::LicenseRequired;

    const Path path = record_path(state_dir_, spec.name);
    if (!path_exists(path)) return Result<ModelRecord>::success(std::move(record));

    std::string text;
    if (!read_text_file(path, &text)) {
        return Result<ModelRecord>::failure("cannot read " + path.string());
    }
    if (text.size() > kMaxStateBytes) {
        return Result<ModelRecord>::failure(path.string() + " is implausibly large");
    }

    try {
        const nlohmann::json root = nlohmann::json::parse(text);
        if (root.at("format_version").get<uint32_t>() != kStateFormatVersion) {
            return Result<ModelRecord>::failure(path.string() + ": unsupported format_version");
        }
        if (root.at("backend").get<std::string>() != spec.name) {
            return Result<ModelRecord>::failure(path.string() + ": record belongs to another backend");
        }
        if (!model_state_from_name(root.at("state").get<std::string>(), &record.state)) {
            return Result<ModelRecord>::failure(path.string() + ": unknown state");
        }
        record.message = root.value("message", std::string());
        record.source_sha256 = root.value("source_sha256", std::string());
        record.source_bytes = root.value("source_bytes", uint64_t{0});
        record.downloaded_bytes = root.value("downloaded_bytes", uint64_t{0});
        record.source_path = root.value("source_path", std::string());
        record.gguf_path = root.value("gguf_path", std::string());
        record.converter_version = root.value("converter_version", spec.converter_version);
        record.updated_at = root.value("updated_at", std::string());
    } catch (const std::exception& error) {
        return Result<ModelRecord>::failure(path.string() + ": invalid state file: " + error.what());
    }

    // A crash leaves one of the transient states behind. Downloading is safely
    // resumed from the .part; verifying/converting is restarted from the source.
    if (record.state == ModelState::Downloading) {
        const Path part = (downloads_dir() / spec.source_filename).string() + part_suffix();
        record.downloaded_bytes = size_or_zero(part);
        record.state = ModelState::Accepted;
        record.message = record.downloaded_bytes > 0 ? "download paused" : "";
    } else if (record.state == ModelState::Verifying || record.state == ModelState::Converting) {
        record.state = ModelState::Failed;
        record.message = "interrupted while " +
                         std::string(record.state == ModelState::Verifying ? "verifying" : "converting") +
                         "; run the fetch again";
    }

    if (record.state == ModelState::LicenseRequired && license_accepted(spec.name)) {
        record.state = ModelState::Accepted;
        record.message.clear();
    }

    // The GGUF is the product of the conversion. On every load, confirm that it
    // is still this backend's file for this source checkpoint: a mismatch means
    // a truncated, edited or stale runtime file.
    if (record.state == ModelState::Ready) {
        const Path gguf = record.gguf_path.empty() ? gguf_path(spec.name) : record.gguf_path;
        const Status check = verify_gguf(spec.name, gguf);
        if (!check.ok()) {
            record.state = ModelState::Failed;
            record.message = check.message();
            record.gguf_path = gguf;
            save_record(record);  // best effort; the in-memory record is authoritative
        } else {
            record.gguf_path = gguf;
        }
    }
    return Result<ModelRecord>::success(std::move(record));
}

Status ModelManager::save_record(const ModelRecord& record) {
    nlohmann::json root;
    root["format_version"] = kStateFormatVersion;
    root["backend"] = record.backend;
    root["state"] = model_state_name(record.state);
    root["message"] = record.message;
    root["source_sha256"] = record.source_sha256;
    root["source_bytes"] = record.source_bytes;
    root["downloaded_bytes"] = record.downloaded_bytes;
    root["source_path"] = record.source_path.string();
    root["gguf_path"] = record.gguf_path.string();
    root["converter_version"] = record.converter_version;
    root["updated_at"] = record.updated_at;
    const std::string text = root.dump(2) + "\n";

    Status directory = ensure_directory(models_dir());
    if (!directory.ok()) return directory;

    const Path path = record_path(state_dir_, record.backend);
    const Path temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return Status::error("cannot open " + temporary.string() + " for writing");
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) return Status::error("failed while writing " + temporary.string());
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        remove_if_present(temporary);
        return Status::error("cannot replace " + path.string() + ": " + error.message());
    }
    return Status::success();
}

Status ModelManager::fail(ModelRecord& record, const std::string& message) {
    record.state = ModelState::Failed;
    record.message = message;
    record.updated_at = current_timestamp_utc();
    const Status saved = save_record(record);
    if (!saved.ok()) return Status::error(message + " (state not saved: " + saved.message() + ")");
    return Status::error(message);
}

Result<ModelRecord> ModelManager::status(const std::string& name) {
    const ModelBackendSpec* spec = backend(name);
    if (spec == nullptr) return Result<ModelRecord>::failure("unknown backend '" + name + "'");
    return load_record(*spec);
}

std::vector<ModelRecord> ModelManager::status_all() {
    std::vector<ModelRecord> records;
    records.reserve(specs_.size());
    for (const ModelBackendSpec& spec : specs_) {
        Result<ModelRecord> record = load_record(spec);
        if (record.ok()) {
            records.push_back(record.take());
        } else {
            ModelRecord failed;
            failed.backend = spec.name;
            failed.state = ModelState::Failed;
            failed.message = record.message();
            records.push_back(std::move(failed));
        }
    }
    return records;
}

Status ModelManager::verify_gguf(const std::string& name, const Path& gguf) const {
    const ModelBackendSpec* spec = backend(name);
    if (spec == nullptr) return Status::error("unknown backend '" + name + "'");

    const Result<GgufHeader> header = read_gguf_header(gguf);
    if (!header.ok()) return Status::error(header.message());
    if (header.value().tensor_count <= 0) {
        return Status::error(gguf.string() + ": the GGUF holds no tensors");
    }

    const auto architecture = header.value().strings.find("general.architecture");
    if (architecture == header.value().strings.end()) {
        return Status::error(gguf.string() + ": missing general.architecture");
    }
    if (architecture->second != spec->gguf_architecture) {
        return Status::error(gguf.string() + ": architecture is '" + architecture->second +
                             "', expected '" + spec->gguf_architecture + "'");
    }

    const std::string prefix = spec->name + ".";
    const auto backend_name = header.value().strings.find(prefix + "backend");
    if (backend_name == header.value().strings.end()) {
        return Status::error(gguf.string() + ": missing " + prefix + "backend");
    }
    if (backend_name->second != spec->name) {
        return Status::error(gguf.string() + ": backend is '" + backend_name->second +
                             "', expected '" + spec->name + "'");
    }

    const auto source_sha = header.value().strings.find(prefix + "source_sha256");
    if (source_sha == header.value().strings.end()) {
        return Status::error(gguf.string() + ": missing " + prefix + "source_sha256");
    }
    if (source_sha->second != spec->source_sha256) {
        return Status::error(gguf.string() + ": source SHA-256 is " + source_sha->second +
                             ", expected the pinned " + spec->source_sha256);
    }

    const auto version = header.value().integers.find(prefix + "converter_version");
    if (version == header.value().integers.end()) {
        return Status::error(gguf.string() + ": missing " + prefix + "converter_version");
    }
    if (version->second != spec->converter_version) {
        return Status::error(gguf.string() + ": converter version is " +
                             std::to_string(version->second) + ", expected " +
                             std::to_string(spec->converter_version));
    }
    return Status::success();
}

Status ModelManager::convert_and_store(const ModelBackendSpec& spec, const Path& source,
                                       ModelRecord& record, Progress& progress) {
    record.state = ModelState::Converting;
    record.message.clear();
    record.source_path = source;
    record.source_sha256 = spec.source_sha256;
    record.source_bytes = spec.source_bytes;
    record.downloaded_bytes = spec.source_bytes;
    record.converter_version = spec.converter_version;
    record.updated_at = current_timestamp_utc();
    Status saved = save_record(record);
    if (!saved.ok()) return saved;

    progress.report(0.80f, "converting");
    const Path output = gguf_path(spec.name);
    const Path temporary = output.string() + part_suffix();
    remove_if_present(temporary);

    Progress inner([&progress](float fraction, const std::string& stage) {
        progress.report(0.80f + 0.19f * fraction, stage);
    });

    Result<ConvertStats> converted = Result<ConvertStats>::failure("no converter configured");
    if (converter_) {
        converted = converter_(spec, source, temporary, inner);
    } else if (spec.name == "da2") {
        ConvertOptions options;
        options.backend = spec.name;
        converted = convert_depth_anything_v2(source, temporary, options, inner);
    } else {
        ConvertOptions options;
        options.backend = spec.name;
        converted = convert_checkpoint(source, temporary, options, inner);
    }
    if (!converted.ok()) {
        remove_if_present(temporary);
        return fail(record, "conversion failed: " + converted.message());
    }

    const Status check = verify_gguf(spec.name, temporary);
    if (!check.ok()) {
        remove_if_present(temporary);
        return fail(record, "the converted file failed the metadata check: " + check.message());
    }

    std::error_code error;
    std::filesystem::rename(temporary, output, error);
    if (error) {
        remove_if_present(temporary);
        return fail(record, "cannot move the converted file into place: " + error.message());
    }

    record.state = ModelState::Ready;
    record.message.clear();
    record.gguf_path = output;
    record.updated_at = current_timestamp_utc();
    saved = save_record(record);
    if (!saved.ok()) return saved;
    progress.report(1.0f, "ready");
    return Status::success();
}

Status ModelManager::import_verified(const ModelBackendSpec& spec, const Path& file, bool move_source,
                                     Progress& progress) {
    Result<ModelRecord> loaded = load_record(spec);
    if (!loaded.ok()) return Status::error(loaded.message());
    ModelRecord record = loaded.take();

    if (!path_exists(file)) return fail(record, "no such file: " + file.string());
    if (file.filename() != spec.source_filename) {
        return fail(record, file.string() + ": expected the official file name '" +
                                spec.source_filename + "' for backend '" + spec.name + "'");
    }

    const uint64_t bytes = size_or_zero(file);
    if (bytes != spec.source_bytes) {
        return fail(record, file.string() + ": " + std::to_string(bytes) +
                                " bytes, expected the pinned " +
                                std::to_string(spec.source_bytes));
    }

    progress.report(0.70f, "verifying");
    const Result<std::string> digest = sha256_file(file);
    if (!digest.ok()) return fail(record, digest.message());
    if (digest.value() != spec.source_sha256) {
        // The source file is the user's copy: leave it in place so it can be
        // replaced deliberately, unlike a partial download of ours.
        return fail(record, file.string() + ": SHA-256 " + digest.value() +
                                " does not match the pinned " + spec.source_sha256);
    }

    record.state = ModelState::Verifying;
    record.message.clear();
    record.source_sha256 = spec.source_sha256;
    record.source_bytes = spec.source_bytes;
    record.downloaded_bytes = spec.source_bytes;
    record.updated_at = current_timestamp_utc();
    Status saved = save_record(record);
    if (!saved.ok()) return saved;

    const Path destination = downloads_dir() / spec.source_filename;
    std::error_code error;
    const bool same_file =
        path_exists(destination) && std::filesystem::equivalent(file, destination, error) && !error;
    if (!same_file) {
        const Status moved = move_or_copy(file, destination, move_source);
        if (!moved.ok()) return fail(record, moved.message());
    }
    return convert_and_store(spec, destination, record, progress);
}

Status ModelManager::import(const std::string& name, const Path& file, Progress& progress) {
    const ModelBackendSpec* spec = backend(name);
    if (spec == nullptr) return Status::error("unknown backend '" + name + "'");

    // The licence gate covers the offline path too: converting is making a
    // derivative, and spec rule 3 requires the acceptance first.
    if (!spec->license_url.empty() && !license_accepted(spec->name)) {
        const std::string message = "the " + spec->license_name +
                                    " licence has not been accepted for '" + spec->name + "'";
        Result<ModelRecord> loaded = load_record(*spec);
        if (loaded.ok()) {
            ModelRecord record = loaded.take();
            record.state = ModelState::LicenseRequired;
            record.message = message;
            record.updated_at = current_timestamp_utc();
            const Status saved = save_record(record);
            if (!saved.ok()) return saved;
        }
        return Status::error(message);
    }
    return import_verified(*spec, file, false, progress);
}

Status ModelManager::import_file(const Path& file, Progress& progress) {
    const std::string filename = file.filename().string();
    for (const ModelBackendSpec& spec : specs_) {
        if (spec.source_filename == filename) return import(spec.name, file, progress);
    }
    std::string expected;
    for (const ModelBackendSpec& spec : specs_) {
        if (!expected.empty()) expected += " or ";
        expected += "'" + spec.source_filename + "' (" + spec.name + ")";
    }
    return Status::error(file.string() + ": unknown model file name; expected " + expected);
}

Status ModelManager::fetch(const std::string& name, Progress& progress,
                           std::function<bool()> cancelled) {
    const ModelBackendSpec* spec = backend(name);
    if (spec == nullptr) return Status::error("unknown backend '" + name + "'");

    Result<ModelRecord> loaded = load_record(*spec);
    if (!loaded.ok()) return Status::error(loaded.message());
    ModelRecord record = loaded.take();
    if (record.state == ModelState::Ready) {
        progress.report(1.0f, "ready");
        return Status::success();
    }

    // Spec rule 3: no download before the licence is accepted.
    if (!spec->license_url.empty() && !license_accepted(spec->name)) {
        record.state = ModelState::LicenseRequired;
        record.message = "the " + spec->license_name + " licence has not been accepted for '" +
                         spec->name + "'";
        record.updated_at = current_timestamp_utc();
        const Status saved = save_record(record);
        if (!saved.ok()) return saved;
        return Status::error(record.message);
    }

    record.state = ModelState::Accepted;
    record.message.clear();
    record.updated_at = current_timestamp_utc();
    Status saved = save_record(record);
    if (!saved.ok()) return saved;
    progress.report(0.02f, "checking the checkpoint");

    // A source left by an earlier run is reused when it still matches the pin;
    // otherwise it is deleted so the download starts clean.
    const Path source = downloads_dir() / spec->source_filename;
    if (path_exists(source)) {
        if (size_or_zero(source) == spec->source_bytes) {
            const Result<std::string> digest = sha256_file(source);
            if (digest.ok() && digest.value() == spec->source_sha256) {
                return convert_and_store(*spec, source, record, progress);
            }
        }
        remove_if_present(source);
    }

    // Offline path: <state>/inbox/<official name>.
    const Path inbox_file = inbox_dir() / spec->source_filename;
    if (path_exists(inbox_file)) {
        // The pickup moves the file into downloads/; a wrong hash leaves the
        // inbox copy untouched and records the failure.
        return import_verified(*spec, inbox_file, true, progress);
    }

    const Path part = source.string() + part_suffix();
    uint64_t offset = size_or_zero(part);
    if (offset > spec->source_bytes) {
        remove_if_present(part);  // a corrupt leftover would make the resume invalid
        offset = 0;
    }

    // A complete .part left by a crash between the last byte and the rename is
    // verified directly: asking for Range: bytes=<size>- would fail with 416.
    if (offset < spec->source_bytes) {
        if (fetcher_ == nullptr) {
            return fail(record, "no HTTP fetcher; put '" + spec->source_filename + "' in " +
                                    inbox_dir().string() + " for the offline path");
        }

        const Result<HttpResource> head = fetcher_->head(spec->source_url);
        if (!head.ok()) {
            return fail(record, "cannot reach " + spec->source_url + ": " + head.message());
        }
        if (head.value().content_length != spec->source_bytes) {
            return fail(record, spec->source_url + " reports " +
                                    std::to_string(head.value().content_length) +
                                    " bytes, the pinned checkpoint is " +
                                    std::to_string(spec->source_bytes) + " bytes");
        }

        record.state = ModelState::Downloading;
        record.downloaded_bytes = offset;
        record.updated_at = current_timestamp_utc();
        saved = save_record(record);
        if (!saved.ok()) return saved;
        progress.report(0.05f, offset > 0 ? "resuming the download" : "downloading");

        Progress download_progress([&progress](float fraction, const std::string& stage) {
            progress.report(0.05f + 0.65f * fraction, stage);
        });
        FetchRequest request;
        request.url = spec->source_url;
        request.destination = part;
        request.offset = offset;
        request.total_bytes = spec->source_bytes;
        request.progress = &download_progress;
        request.cancelled = cancelled;

        const Status downloaded = fetcher_->download(request);
        if (!downloaded.ok()) {
            record.downloaded_bytes = size_or_zero(part);
            return fail(record, "download failed: " + downloaded.message());  // kept for resume
        }
    }

    progress.report(0.70f, "verifying");
    record.state = ModelState::Verifying;
    record.message.clear();
    record.downloaded_bytes = size_or_zero(part);
    record.updated_at = current_timestamp_utc();
    saved = save_record(record);
    if (!saved.ok()) return saved;

    const uint64_t downloaded_bytes = size_or_zero(part);
    if (downloaded_bytes != spec->source_bytes) {
        remove_if_present(part);
        return fail(record, "downloaded " + std::to_string(downloaded_bytes) + " bytes, expected " +
                                std::to_string(spec->source_bytes) +
                                "; the partial file was removed");
    }
    const Result<std::string> digest = sha256_file(part);
    if (!digest.ok()) {
        remove_if_present(part);
        return fail(record, digest.message());
    }
    if (digest.value() != spec->source_sha256) {
        remove_if_present(part);
        return fail(record, "SHA-256 mismatch: downloaded " + digest.value() + ", expected the pinned " +
                                spec->source_sha256 + "; the partial file was removed");
    }

    std::error_code error;
    std::filesystem::rename(part, source, error);
    if (error) {
        return fail(record, "cannot move the verified download into place: " + error.message());
    }
    return convert_and_store(*spec, source, record, progress);
}

}  // namespace splat
