#include "model/license.h"

#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <system_error>

#include <nlohmann/json.hpp>

namespace splat {
namespace {

constexpr uint32_t kLicenseFormatVersion = 1;
constexpr uint64_t kMaxLicenseStateBytes = 1u << 20;

std::string timestamp_from(std::time_t seconds) {
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    char text[32] = {};
    if (std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) return {};
    return text;
}

}  // namespace

Path license_state_path(const Path& state_dir) { return state_dir / "licenses.json"; }

bool is_sha256_hex(const std::string& text) {
    if (text.size() != 64) return false;
    for (char character : text) {
        const bool digit = character >= '0' && character <= '9';
        const bool lower = character >= 'a' && character <= 'f';
        if (!digit && !lower) return false;
    }
    return true;
}

Result<LicenseState> read_license_state(const Path& state_dir) {
    const Path path = license_state_path(state_dir);
    std::error_code file_error;
    if (!std::filesystem::exists(path, file_error)) {
        if (file_error) {
            return Result<LicenseState>::failure("cannot stat " + path.string() + ": " +
                                                 file_error.message());
        }
        return Result<LicenseState>::success(LicenseState{});
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) return Result<LicenseState>::failure("cannot open " + path.string());
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (text.size() > kMaxLicenseStateBytes) {
        return Result<LicenseState>::failure(path.string() + " is implausibly large");
    }

    try {
        const nlohmann::json root = nlohmann::json::parse(text);
        if (!root.is_object()) return Result<LicenseState>::failure(path.string() + ": root is not an object");

        LicenseState state;
        state.format_version = root.at("format_version").get<uint32_t>();
        if (state.format_version != kLicenseFormatVersion) {
            return Result<LicenseState>::failure(path.string() + ": unsupported format_version " +
                                                 std::to_string(state.format_version));
        }
        for (const nlohmann::json& entry : root.at("accepted")) {
            LicenseAcceptance acceptance;
            acceptance.backend = entry.at("backend").get<std::string>();
            acceptance.license_sha256 = entry.at("license_sha256").get<std::string>();
            acceptance.accepted_at = entry.at("accepted_at").get<std::string>();
            if (acceptance.backend.empty()) {
                return Result<LicenseState>::failure(path.string() + ": acceptance without a backend");
            }
            if (!is_sha256_hex(acceptance.license_sha256)) {
                return Result<LicenseState>::failure(path.string() + ": acceptance for '" +
                                                     acceptance.backend +
                                                     "' has no valid license_sha256");
            }
            if (acceptance.accepted_at.empty()) {
                return Result<LicenseState>::failure(path.string() + ": acceptance for '" +
                                                     acceptance.backend + "' has no accepted_at");
            }
            state.accepted.push_back(std::move(acceptance));
        }
        return Result<LicenseState>::success(std::move(state));
    } catch (const std::exception& error) {
        return Result<LicenseState>::failure(path.string() + ": invalid licenses.json: " + error.what());
    }
}

Status write_license_state(const Path& state_dir, const LicenseState& state) {
    nlohmann::json accepted = nlohmann::json::array();
    for (const LicenseAcceptance& entry : state.accepted) {
        accepted.push_back({{"backend", entry.backend},
                            {"license_sha256", entry.license_sha256},
                            {"accepted_at", entry.accepted_at}});
    }
    nlohmann::json root;
    root["format_version"] = kLicenseFormatVersion;
    root["accepted"] = std::move(accepted);
    const std::string text = root.dump(2) + "\n";

    std::error_code file_error;
    std::filesystem::create_directories(state_dir, file_error);
    if (file_error) {
        return Status::error("cannot create " + state_dir.string() + ": " + file_error.message());
    }

    const Path path = license_state_path(state_dir);
    const Path temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return Status::error("cannot open " + temporary.string() + " for writing");
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) return Status::error("failed while writing " + temporary.string());
    }
    std::filesystem::rename(temporary, path, file_error);
    if (file_error) {
        std::filesystem::remove(temporary, file_error);
        return Status::error("cannot replace " + path.string());
    }
    return Status::success();
}

bool license_accepted(const LicenseState& state, const std::string& backend,
                      const std::string& license_sha256) {
    for (const LicenseAcceptance& entry : state.accepted) {
        if (entry.backend == backend && entry.license_sha256 == license_sha256) return true;
    }
    return false;
}

Status record_license_acceptance(const Path& state_dir, const LicenseAcceptance& acceptance) {
    if (acceptance.backend.empty()) return Status::error("license acceptance needs a backend");
    if (!is_sha256_hex(acceptance.license_sha256)) {
        return Status::error("license acceptance for '" + acceptance.backend +
                             "' needs a lowercase hex SHA-256 of the licence text");
    }
    if (acceptance.accepted_at.empty()) return Status::error("license acceptance needs a timestamp");

    Result<LicenseState> state = read_license_state(state_dir);
    if (!state.ok()) return Status::error(state.message());

    LicenseState updated = state.value();
    bool replaced = false;
    for (LicenseAcceptance& entry : updated.accepted) {
        if (entry.backend == acceptance.backend) {
            entry = acceptance;
            replaced = true;
            break;
        }
    }
    if (!replaced) updated.accepted.push_back(acceptance);
    return write_license_state(state_dir, updated);
}

std::string current_timestamp_utc() { return timestamp_from(std::time(nullptr)); }

}  // namespace splat
