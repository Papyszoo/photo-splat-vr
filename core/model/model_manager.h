#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "common/path.h"
#include "common/progress.h"
#include "common/status.h"
#include "model/converter.h"
#include "model/http_fetch.h"

namespace splat {

// C1 state machine, one instance per backend.
//
//   licence not accepted -> accepted -> downloading (resume from .part) or
//   inbox pickup -> verifying (pinned SHA-256) -> converting (the C1 converter)
//   -> ready, with Failed carrying a message at any step.
//
// Every transition is written to the state directory, so a restart resumes the
// same place. A Ready record is revalidated by reading the GGUF metadata, and
// is demoted to Failed when the file is missing or does not belong to this
// backend and source checkpoint.
enum class ModelState {
    LicenseRequired = 0,
    Accepted,
    Downloading,
    Verifying,
    Converting,
    Ready,
    Failed,
};

const char* model_state_name(ModelState state);
bool model_state_from_name(const std::string& name, ModelState* state);

// Pinned identity of one backend's source checkpoint (spec rules 1, 2 and C4:
// the fallback is fetched exactly like SHARP).
struct ModelBackendSpec {
    std::string name;             // BackendInfo::name, "sharp" or "da2"
    std::string display_name;     // UI label
    std::string source_filename;  // official file name; the inbox requires it
    std::string source_url;       // official download URL
    std::string source_sha256;    // pinned, lowercase hex
    uint64_t source_bytes = 0;    // pinned size
    std::string gguf_architecture;  // general.architecture of the converted file
    uint32_t converter_version = 1;  // pinned converter output version
    // Empty for a backend whose terms need no acceptance (the Apache-2.0
    // fallback). Otherwise the URL of the licence text to show before the first
    // download; the text is fetched at runtime, never bundled (spec rule 3).
    std::string license_url;
    std::string license_name;
};

// Built-in pinned specs. SHARP has a licence gate; the fallback does not.
const ModelBackendSpec& sharp_model_spec();
const ModelBackendSpec& da2_model_spec();

struct ModelRecord {
    std::string backend;
    ModelState state = ModelState::Failed;
    std::string message;         // failure reason, or a short status note
    std::string source_sha256;   // hash that was verified
    uint64_t source_bytes = 0;
    uint64_t downloaded_bytes = 0;  // bytes present in downloads/ (or .part)
    Path source_path;               // verified checkpoint, once downloaded
    Path gguf_path;                 // converted runtime file, once ready
    uint32_t converter_version = 0;
    std::string updated_at;         // ISO-8601 UTC
};

struct ModelManagerOptions {
    Path state_dir;              // holds licenses.json, models/, downloads/, inbox/
    IHttpFetcher* fetcher = nullptr;  // null: offline (inbox/import only)
    // Empty: the built-in sharp + da2 specs. Tests substitute a small file.
    std::vector<ModelBackendSpec> specs;
    // Empty: the real converter (convert_checkpoint / convert_depth_anything_v2).
    // A test seam, so the state machine can run without a 2.8 GB checkpoint.
    using Converter = std::function<Result<ConvertStats>(const ModelBackendSpec& spec,
                                                         const Path& source, const Path& out,
                                                         Progress& progress)>;
    Converter converter;
};

// Layout under the state directory:
//   licenses.json            licence acceptances (core/model/license.*)
//   models/<backend>.json    persisted state machine record
//   downloads/<filename>     verified source checkpoint
//   downloads/<filename>.part  resumable partial download
//   models/<backend>.gguf    converted runtime file
//   inbox/                   offline drop folder (FTP/USB)
class ModelManager {
public:
    explicit ModelManager(ModelManagerOptions options);

    // nullptr when the name is not one of the configured backends.
    const ModelBackendSpec* backend(const std::string& name) const;
    std::vector<std::string> backend_names() const;

    const Path& state_dir() const { return state_dir_; }
    Path inbox_dir() const;
    Path downloads_dir() const;
    Path models_dir() const;
    Path gguf_path(const std::string& backend) const;

    // Reads the persisted record (or the initial state when there is none) and
    // revalidates a Ready record against the GGUF metadata. Unknown backend is
    // an error.
    Result<ModelRecord> status(const std::string& backend);
    std::vector<ModelRecord> status_all();

    // True when licenses.json holds an acceptance for this backend. A missing
    // or unreadable file counts as not accepted (fail closed).
    bool license_accepted(const std::string& backend) const;

    // Downloads the backend's licence text. Fails for a backend without a gate.
    Result<std::string> fetch_license_text(const std::string& backend);

    // Records the acceptance of exactly this text (its SHA-256 is stored).
    Status accept_license(const std::string& backend, const std::string& text);

    // Runs download (or inbox pickup) -> verify -> convert -> ready, resuming a
    // .part file. A download failure keeps the .part for the next call; a hash
    // mismatch removes it. A non-empty `cancelled` is handed to the fetcher,
    // which polls it during the transfer; a cancelled download fails like any
    // other interrupted one and keeps the .part for a later resume.
    Status fetch(const std::string& backend, Progress& progress,
                 std::function<bool()> cancelled = {});

    // Imports an already-downloaded checkpoint: checks the pinned size and
    // SHA-256, then converts. `backend` must own the file. The source file is
    // copied, not moved.
    Status import(const std::string& backend, const Path& file, Progress& progress);

    // import() with the backend inferred from the file name (sharp_2572gikvuh.pt
    // or model.safetensors). Unknown names are an error.
    Status import_file(const Path& file, Progress& progress);

    // Reads the GGUF header (no tensor allocation) and checks general.
    // architecture, <backend>.backend, <backend>.source_sha256 and
    // <backend>.converter_version.
    Status verify_gguf(const std::string& backend, const Path& gguf) const;

private:
    Result<ModelRecord> load_record(const ModelBackendSpec& spec);
    Status save_record(const ModelRecord& record);
    Status fail(ModelRecord& record, const std::string& message);
    Status convert_and_store(const ModelBackendSpec& spec, const Path& source, ModelRecord& record,
                             Progress& progress);
    Status import_verified(const ModelBackendSpec& spec, const Path& file, bool move_source,
                           Progress& progress);

    Path state_dir_;
    IHttpFetcher* fetcher_ = nullptr;
    std::vector<ModelBackendSpec> specs_;
    ModelManagerOptions::Converter converter_;
};

}  // namespace splat
