#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "model/license.h"
#include "test_util.h"

using splat::LicenseAcceptance;
using splat::LicenseState;
using splat::Status;

namespace {

constexpr char kLicenseSha[] = "9c1f2e3d4b5a69788796a5b4c3d2e1f00f1e2d3c4b5a69788796a5b4c3d2e1f0";
constexpr char kOtherSha[] = "1111111111111111111111111111111111111111111111111111111111111111";

LicenseAcceptance make_acceptance(const std::string& backend, const std::string& sha) {
    LicenseAcceptance acceptance;
    acceptance.backend = backend;
    acceptance.license_sha256 = sha;
    acceptance.accepted_at = "2026-10-25T12:00:00Z";
    return acceptance;
}

}  // namespace

TEST_CASE("license state: a missing file means nothing accepted yet") {
    splat_test::TempDir temp("license-empty");
    const auto state = splat::read_license_state(temp.path());
    REQUIRE_MESSAGE(state.ok(), state.message());
    CHECK(state.value().accepted.empty());
    CHECK(state.value().format_version == 1);
    CHECK_FALSE(splat::license_accepted(state.value(), "sharp", kLicenseSha));
    CHECK_FALSE(std::filesystem::exists(splat::license_state_path(temp.path())));
}

TEST_CASE("license state: an acceptance round trips") {
    splat_test::TempDir temp("license-roundtrip");
    const Status written = splat::record_license_acceptance(temp.path(), make_acceptance("sharp", kLicenseSha));
    REQUIRE_MESSAGE(written.ok(), written.message());
    REQUIRE(std::filesystem::exists(splat::license_state_path(temp.path())));

    const auto state = splat::read_license_state(temp.path());
    REQUIRE_MESSAGE(state.ok(), state.message());
    REQUIRE(state.value().accepted.size() == 1);
    CHECK(state.value().accepted[0].backend == "sharp");
    CHECK(state.value().accepted[0].license_sha256 == kLicenseSha);
    CHECK(state.value().accepted[0].accepted_at == "2026-10-25T12:00:00Z");

    // The gate compares backend and licence text hash, not just the backend.
    CHECK(splat::license_accepted(state.value(), "sharp", kLicenseSha));
    CHECK_FALSE(splat::license_accepted(state.value(), "sharp", kOtherSha));
    CHECK_FALSE(splat::license_accepted(state.value(), "fallback", kLicenseSha));

    // The file on disk is the documented JSON shape.
    std::ifstream file(splat::license_state_path(temp.path()));
    nlohmann::json root;
    file >> root;
    CHECK(root.at("format_version").get<int>() == 1);
    REQUIRE(root.at("accepted").is_array());
    REQUIRE(root.at("accepted").size() == 1);
    CHECK(root.at("accepted")[0].at("backend").get<std::string>() == "sharp");
    CHECK(root.at("accepted")[0].at("license_sha256").get<std::string>() == kLicenseSha);
}

TEST_CASE("license state: a new acceptance replaces the backend's earlier one") {
    splat_test::TempDir temp("license-replace");
    REQUIRE(splat::record_license_acceptance(temp.path(), make_acceptance("sharp", kLicenseSha)).ok());
    REQUIRE(splat::record_license_acceptance(temp.path(), make_acceptance("fallback", kOtherSha)).ok());

    LicenseAcceptance updated = make_acceptance("sharp", kOtherSha);
    updated.accepted_at = "2026-10-26T08:30:00Z";
    REQUIRE(splat::record_license_acceptance(temp.path(), updated).ok());

    const auto state = splat::read_license_state(temp.path());
    REQUIRE_MESSAGE(state.ok(), state.message());
    REQUIRE(state.value().accepted.size() == 2);  // one per backend
    CHECK(splat::license_accepted(state.value(), "sharp", kOtherSha));
    CHECK_FALSE(splat::license_accepted(state.value(), "sharp", kLicenseSha));
    CHECK(splat::license_accepted(state.value(), "fallback", kOtherSha));
}

TEST_CASE("license state: malformed files are rejected") {
    splat_test::TempDir temp("license-invalid");
    const auto path = splat::license_state_path(temp.path());

    {
        std::ofstream file(path);
        file << "not json";
    }
    CHECK_FALSE(splat::read_license_state(temp.path()).ok());

    {
        std::ofstream file(path);
        file << R"({"format_version": 2, "accepted": []})";
    }
    CHECK_FALSE(splat::read_license_state(temp.path()).ok());

    {
        std::ofstream file(path);
        file << R"({"format_version": 1, "accepted": [{"backend": "sharp",
                  "license_sha256": "abc", "accepted_at": "2026-10-25T12:00:00Z"}]})";
    }
    const auto bad_hash = splat::read_license_state(temp.path());
    REQUIRE_FALSE(bad_hash.ok());
    CHECK(bad_hash.message().find("license_sha256") != std::string::npos);

    {
        std::ofstream file(path);
        file << R"({"format_version": 1, "accepted": [{"backend": "sharp",
                  "license_sha256": "9c1f2e3d4b5a69788796a5b4c3d2e1f00f1e2d3c4b5a69788796a5b4c3d2e1f0",
                  "accepted_at": ""}]})";
    }
    CHECK_FALSE(splat::read_license_state(temp.path()).ok());
}

TEST_CASE("license state: refusals are explicit") {
    splat_test::TempDir temp("license-refuse");
    LicenseAcceptance no_backend = make_acceptance("", kLicenseSha);
    CHECK_FALSE(splat::record_license_acceptance(temp.path(), no_backend).ok());

    LicenseAcceptance bad_hash = make_acceptance("sharp", "ABC");
    CHECK_FALSE(splat::record_license_acceptance(temp.path(), bad_hash).ok());

    LicenseAcceptance no_time = make_acceptance("sharp", kLicenseSha);
    no_time.accepted_at.clear();
    CHECK_FALSE(splat::record_license_acceptance(temp.path(), no_time).ok());
    CHECK_FALSE(std::filesystem::exists(splat::license_state_path(temp.path())));
}

TEST_CASE("license state: timestamps are ISO-8601 UTC") {
    const std::string now = splat::current_timestamp_utc();
    REQUIRE(now.size() == 20);
    CHECK(now[4] == '-');
    CHECK(now[7] == '-');
    CHECK(now[10] == 'T');
    CHECK(now[13] == ':');
    CHECK(now[16] == ':');
    CHECK(now[19] == 'Z');
    CHECK(splat::is_sha256_hex(kLicenseSha));
    CHECK_FALSE(splat::is_sha256_hex(""));
    CHECK_FALSE(splat::is_sha256_hex(std::string(64, 'G')));
    CHECK_FALSE(splat::is_sha256_hex(std::string(63, 'a')));
}
