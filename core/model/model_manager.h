#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "common/path.h"
#include "common/progress.h"
#include "common/status.h"
#include "model/converter.h"
#include "model/http_fetch.h"
#include "model/segmented_download.h"
#include "model/stream_converter.h"

namespace splat {

// Where the source download sits on the one progress bar fetch() reports
// (download 0.05 .. 0.70, then verify, then convert). The models row turns the
// bar back into bytes for its time estimate.
inline constexpr float kFetchDownloadStart = 0.05f;
inline constexpr float kFetchDownloadSpan = 0.65f;

// The one stall deadline of the download pipeline: a transfer that moves no
// byte for this long is aborted (the model service's watchdog calls
// abort_transfer()). The console's sceHttp call can block without honouring its
// receive timeout, so a deadline alone is not enough - the abort is what ends
// it. Shorter deadlines would mistake a slow link for a stall (a 2.8 GB
// checkpoint at 3 MB/s changes its percent about once per 9 s).
inline constexpr double kTransferStallSeconds = 60.0;

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
    // `source_sha256` is the digest the manager has already checked against the
    // pin (hash-while-downloading, a resume check or an import), so the
    // converter never reads the file a second time just to hash it.
    using Converter = std::function<Result<ConvertStats>(
        const ModelBackendSpec& spec, const Path& source, const Path& out,
        const std::string& source_sha256, Progress& progress)>;
    Converter converter;
    // A download with at least this much left to fetch, from a server that offers
    // byte ranges, runs as `segment_count` parallel ranged requests
    // (core/model/segmented_download.h); anything else is one GET. A test seam.
    // UINT64_MAX turns segmentation off for this manager: every download is one
    // plain GET (no Range header), and a .segments layout an older build left is
    // discarded instead of being resumed (the console's setting; see
    // platform/ps5/viewer/ui_setup.cpp).
    uint64_t segment_min_bytes = kSegmentedMinBytes;
    int segment_count = kSegmentedCount;
    // How many of those ranged requests may be in flight at once (0: all of
    // them). The console's sceHttp is only proven with one transfer at a time;
    // see SegmentedDownload::max_in_flight.
    int max_parallel_transfers = 0;
    // Convert a torch.save checkpoint from the network bytes instead of storing
    // it: the console's /data write path is the bottleneck, and the streaming
    // path writes only the GGUF (core/model/stream_converter.h). Only SHARP's
    // torch zip is a checkpoint this converter reads; every other backend keeps
    // the download-then-convert path. A streaming download never resumes: an
    // interruption removes the partial GGUF and Retry starts over.
    bool convert_while_downloading = false;
    // One line per phase with its total time and MB/s - download, SHA-256,
    // conversion and the whole fetch - so the next console klog dates each fix
    // (runs/issue-1-crash/sharp-fast-report.md). Empty: no lines. The console
    // wires it to klog in platform/ps5/viewer/ui_setup.cpp.
    std::function<void(const std::string&)> log;
};

// Layout under the state directory:
//   licenses.json            licence acceptances (core/model/license.*)
//   models/<backend>.json    persisted state machine record
//   downloads/<filename>     verified source checkpoint
//   downloads/<filename>.part  resumable partial download
//   downloads/<filename>.part.segments  per-segment resume state of a parallel download
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

    // The network phase of the fetch() running on another thread, for a stall
    // watchdog: active from before the HEAD until the last body byte (not during
    // verify or convert), and the bytes of the file known to be on disk. One
    // fetch at a time is the caller's contract (ModelService enforces it), so a
    // single pair of counters describes the live transfer.
    bool transfer_active() const { return transfer_active_.load(std::memory_order_relaxed); }
    uint64_t transfer_bytes() const {
        return transfer_base_.load(std::memory_order_relaxed) +
               transfer_written_.load(std::memory_order_relaxed);
    }

    // Stops the transfer fetch() is running now, from another thread. Forwarded
    // to the fetcher, whose abort() must make a blocked call fail (see
    // IHttpFetcher::abort); a fetcher that polls `cancelled` needs no override.
    void abort_transfer(FetchAbort reason);

    // True when fetch() would convert this backend from the network body and
    // never store the source checkpoint (core/model/stream_converter.h). The
    // Models free-space gate needs it: only the GGUF is written then.
    bool streams_source(const std::string& backend) const;

    // Reclaims the streaming backend's leftovers on a worker (the model
    // service's stall watch at startup), never the render thread: a partial
    // models/<name>.gguf.part from an attempt that was killed mid-write (no
    // resume exists for it), and the source checkpoint a 0.6.1 install left
    // behind once its GGUF is Ready (the streaming path never reads it again).
    // Returns true when a file was removed.
    bool reclaim_streamed_sources();

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
    // `verified_sha256` is the digest this fetch/import already checked against
    // the pin; the converter uses it instead of hashing the source again.
    Status convert_and_store(const ModelBackendSpec& spec, const Path& source,
                             const std::string& verified_sha256, ModelRecord& record,
                             Progress& progress);
    // The convert-while-downloading path: one plain GET whose body feeds the
    // StreamConverter, the digest checked against the pin at the end, the GGUF
    // verified and renamed. No .pt (or .part) is written.
    Status fetch_streaming(const ModelBackendSpec& spec, ModelRecord& record, Progress& progress,
                           const std::function<bool()>& cancelled);
    // verify_gguf on a finished conversion, rename it into place and record
    // Ready: the last three steps of both the file path and the streaming path.
    Status store_converted(const ModelBackendSpec& spec, const Path& temporary,
                           ModelRecord& record, Progress& progress);
    // The streaming path never needs the source checkpoint again: a .pt (and its
    // .part/.segments) that an older run or the inbox left behind is removed
    // once the model has been converted, so /data keeps only the GGUF. Returns
    // true when something was removed. Only the streaming backend is affected.
    bool drop_source_after_streaming(const ModelBackendSpec& spec, const Path& source);
    Status import_verified(const ModelBackendSpec& spec, const Path& file, bool move_source,
                           Progress& progress);
    void log_line(const std::string& text) const;

    Path state_dir_;
    IHttpFetcher* fetcher_ = nullptr;
    std::vector<ModelBackendSpec> specs_;
    ModelManagerOptions::Converter converter_;
    uint64_t segment_min_bytes_ = kSegmentedMinBytes;
    int segment_count_ = kSegmentedCount;
    int max_parallel_transfers_ = 0;
    bool convert_while_downloading_ = false;
    // The live network phase, read by the stall watchdog (see transfer_active).
    std::atomic<bool> transfer_active_{false};
    std::atomic<uint64_t> transfer_base_{0};     // the plain GET's resume offset (0 for segments)
    std::atomic<uint64_t> transfer_written_{0};  // bytes this transfer has written
    std::function<void(const std::string&)> log_;
};

}  // namespace splat
