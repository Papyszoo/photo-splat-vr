#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/path.h"
#include "common/status.h"

namespace splat {

// Spec rule 3: before a backend's weights may be downloaded, the user accepts
// its model licence and the acceptance is recorded locally. This module is only
// the state store; the licence text, the UI and the download live elsewhere.
struct LicenseAcceptance {
    std::string backend;         // BackendInfo::name, e.g. "sharp"
    std::string license_sha256;  // lowercase hex SHA-256 of the licence text shown
    std::string accepted_at;     // ISO-8601 UTC, e.g. "2026-10-25T18:03:11Z"
};

struct LicenseState {
    uint32_t format_version = 1;
    std::vector<LicenseAcceptance> accepted;
};

// <state dir>/licenses.json
Path license_state_path(const Path& state_dir);

// Reads the state file. A missing file is an empty state, not an error: nothing
// has been accepted yet. A malformed file is an error.
Result<LicenseState> read_license_state(const Path& state_dir);

// Writes the state file, creating the directory. The file is replaced
// atomically (temporary file + rename).
Status write_license_state(const Path& state_dir, const LicenseState& state);

// True when `backend` accepted exactly this licence text.
bool license_accepted(const LicenseState& state, const std::string& backend,
                      const std::string& license_sha256);

// Reads, replaces the entry for `backend` (one acceptance per backend) and
// writes the file.
Status record_license_acceptance(const Path& state_dir, const LicenseAcceptance& acceptance);

// Current time as "YYYY-MM-DDTHH:MM:SSZ".
std::string current_timestamp_utc();

// 64 lowercase hex characters.
bool is_sha256_hex(const std::string& text);

}  // namespace splat
