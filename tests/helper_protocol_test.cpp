// W5. Host tests for the splat-helper mailbox protocol: a fabricated capture
// tree shaped like /user/av_contents, the list/copy rules, the copy naming, and
// the refusals (path traversal, symlinks, HDR .jxr, malformed requests).
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "capture/helper_protocol.h"
#include "test_util.h"

namespace {

namespace fs = std::filesystem;
using splat::CaptureInfo;
using splat::HelperResult;
using splat::Path;

void write_file(const Path& path, const std::string& data) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string read_file(const Path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// photo/NPXS40087/CUSA12392/2c4/<name>, i.e. the console layout.
Path capture_path(const Path& root, const std::string& name) {
    return root / "photo" / "NPXS40087" / "CUSA12392" / "2c4" / name;
}

struct CaptureTree {
    explicit CaptureTree(const Path& root) {
        write_file(capture_path(root, "20261025_052805_00338445.jxr"), std::string(64, 'x'));
        write_file(capture_path(root, "20261025_052805_00338445.meta"), "meta");
        write_file(capture_path(root, "20261025_052805_00338445.dat"), std::string(16, 'd'));
        write_file(capture_path(root, "20261025_052805_00338445.ext"), "ext");
        write_file(capture_path(root, "20261024_101112_aabbccdd.jpg"), std::string(128, 'j'));
        write_file(capture_path(root, "20261023_090807_11223344.png"), std::string(256, 'p'));
        write_file(capture_path(root, "20261022_080706_deadbeef.jpeg"), std::string(32, 'J'));
        // A thumbnails subtree under photo/ must never be listed.
        write_file(root / "photo" / "thumbnails" / "NPXS40087" / "20261025_052805_00338445.jxr.jxr",
                   std::string(8, 't'));
    }
};

nlohmann::json answer(const std::string& request, const Path& root, const Path& inbox) {
    return nlohmann::json::parse(splat::handle_request(request, root, inbox), nullptr, false);
}

}  // namespace

TEST_CASE("helper list scans photo/ newest first and flags HDR captures unsupported") {
    splat_test::TempDir dir("helper-tree");
    const CaptureTree tree{dir.path()};

    const std::vector<CaptureInfo> captures = splat::scan_captures(dir.path());
    REQUIRE(captures.size() == 4);

    CHECK(captures[0].name == "20261025_052805_00338445.jxr");
    CHECK(captures[1].name == "20261024_101112_aabbccdd.jpg");
    CHECK(captures[2].name == "20261023_090807_11223344.png");
    CHECK(captures[3].name == "20261022_080706_deadbeef.jpeg");

    CHECK(captures[0].time == "20261025_052805");
    CHECK(captures[0].fmt == "jxr");
    CHECK_FALSE(captures[0].supported);
    CHECK(captures[0].bytes == 64);

    CHECK(captures[1].time == "20261024_101112");
    CHECK(captures[1].fmt == "jpg");
    CHECK(captures[1].supported);
    CHECK(captures[1].bytes == 128);

    CHECK(captures[2].fmt == "png");
    CHECK(captures[2].supported);
    CHECK(captures[3].fmt == "jpg");  // .jpeg is a JPG capture
    CHECK(captures[3].supported);

    for (const CaptureInfo& capture : captures) {
        CHECK(capture.path.string().find("thumbnails") == std::string::npos);
        CHECK(capture.path.is_absolute());
    }
}

TEST_CASE("helper list ignores meta, dat and ext companions") {
    splat_test::TempDir dir("helper-companions");
    const CaptureTree tree{dir.path()};

    const std::vector<CaptureInfo> captures = splat::scan_captures(dir.path());
    for (const CaptureInfo& capture : captures) {
        CHECK(capture.fmt != "meta");
        CHECK(capture.name.find(".meta") == std::string::npos);
        CHECK(capture.name.find(".dat") == std::string::npos);
        CHECK(capture.name.find(".ext") == std::string::npos);
    }
}

TEST_CASE("helper list answers the documented response shape") {
    splat_test::TempDir dir("helper-response");
    const CaptureTree tree{dir.path()};
    const Path inbox = dir.path() / "inbox";

    const nlohmann::json response = answer(R"({"id":42,"op":"list"})", dir.path(), inbox);
    REQUIRE(response.is_object());
    CHECK(response["id"] == 42);
    CHECK(response["ok"] == true);
    CHECK(response["error"] == "");
    REQUIRE(response["captures"].is_array());
    REQUIRE(response["captures"].size() == 4);

    const nlohmann::json& first = response["captures"][0];
    CHECK(first["fmt"] == "jxr");
    CHECK(first["supported"] == false);
    CHECK(first["time"] == "20261025_052805");
    CHECK(first["bytes"] == 64);
    CHECK(first["name"] == "20261025_052805_00338445.jxr");
    CHECK(first["path"].get<std::string>().find("photo/") != std::string::npos);
    CHECK(response["copied"].is_array());
    CHECK(response["copied"].empty());
}

TEST_CASE("helper copy writes capture-<time>-<8hex>.<ext> and never overwrites") {
    splat_test::TempDir dir("helper-copy");
    const CaptureTree tree{dir.path()};
    const Path inbox = dir.path() / "inbox";

    const Path source = capture_path(dir.path(), "20261024_101112_aabbccdd.jpg");
    const HelperResult first = splat::copy_captures(dir.path(), inbox, {source});
    REQUIRE(first.ok);
    CHECK(first.error.empty());
    REQUIRE(first.copied.size() == 1);
    CHECK(first.copied[0].src == source);
    CHECK(first.copied[0].dst == inbox / "capture-20261024_101112-aabbccdd.jpg");
    CHECK(read_file(first.copied[0].dst) == read_file(source));

    // A second copy of the same capture is skipped, and the first stays intact.
    const HelperResult second = splat::copy_captures(dir.path(), inbox, {source});
    CHECK(second.ok);
    CHECK(second.copied.empty());
    CHECK(read_file(inbox / "capture-20261024_101112-aabbccdd.jpg") == std::string(128, 'j'));

    // Each supported format lands under its own extension.
    const HelperResult png =
        splat::copy_captures(dir.path(), inbox, {capture_path(dir.path(), "20261023_090807_11223344.png")});
    REQUIRE(png.copied.size() == 1);
    CHECK(png.copied[0].dst == inbox / "capture-20261023_090807-11223344.png");
    CHECK(fs::file_size(png.copied[0].dst) == 256);
}

TEST_CASE("helper copy answers with the destinations it wrote") {
    splat_test::TempDir dir("helper-copy-json");
    const CaptureTree tree{dir.path()};
    const Path inbox = dir.path() / "inbox";
    const std::string request = nlohmann::json({{"id", 7},
                                                {"op", "copy"},
                                                {"paths", {capture_path(dir.path(), "20261024_101112_aabbccdd.jpg")
                                                               .string()}}})
                                    .dump();

    const nlohmann::json response = answer(request, dir.path(), inbox);
    CHECK(response["id"] == 7);
    CHECK(response["ok"] == true);
    REQUIRE(response["copied"].size() == 1);
    CHECK(response["copied"][0]["src"].get<std::string>().find("aabbccdd.jpg") != std::string::npos);
    CHECK(response["copied"][0]["dst"].get<std::string>().find("capture-20261024_101112-aabbccdd.jpg") !=
          std::string::npos);
    CHECK(response["captures"].empty());
}

TEST_CASE("helper copy refuses a symlink under the root that points outside it") {
    splat_test::TempDir dir("helper-symlink-root");
    splat_test::TempDir outside("helper-symlink-outside");
    const CaptureTree tree{dir.path()};
    const Path inbox = dir.path() / "inbox";

    // The file every link aims at, outside the capture root.
    const Path secret = outside.path() / "20261025_060000_11223344.jpg";
    write_file(secret, std::string(64, 's'));

    // A file link in the leaf folder and a directory link higher up.
    const Path file_link = capture_path(dir.path(), "20261025_060000_11223344.jpg");
    fs::create_symlink(secret, file_link);
    const Path dir_link = dir.path() / "photo" / "outside";
    fs::create_directory_symlink(outside.path(), dir_link);

    for (const Path& link : {file_link, dir_link / secret.filename()}) {
        const HelperResult result = splat::copy_captures(dir.path(), inbox, {link});
        CHECK_FALSE(result.ok);
        CHECK(result.copied.empty());
        CHECK(result.error.find("symlink") != std::string::npos);
        CHECK_FALSE(fs::exists(inbox));
    }

    // A link that points back inside the root is refused as well: a capture is
    // copied under the path the title scanned, never through a link.
    const Path inside_link = capture_path(dir.path(), "20261025_070000_55667788.jpg");
    fs::create_symlink(capture_path(dir.path(), "20261024_101112_aabbccdd.jpg"), inside_link);
    const HelperResult via_inside = splat::copy_captures(dir.path(), inbox, {inside_link});
    CHECK_FALSE(via_inside.ok);
    CHECK(via_inside.copied.empty());
    CHECK(via_inside.error.find("symlink") != std::string::npos);
    CHECK_FALSE(fs::exists(inbox));

    // A normal file in the same tree still copies, and no link target lands in
    // the inbox.
    const HelperResult normal = splat::copy_captures(
        dir.path(), inbox, {capture_path(dir.path(), "20261024_101112_aabbccdd.jpg")});
    REQUIRE(normal.ok);
    REQUIRE(normal.copied.size() == 1);
    CHECK(normal.copied[0].dst == inbox / "capture-20261024_101112-aabbccdd.jpg");
    CHECK(read_file(normal.copied[0].dst) == std::string(128, 'j'));
    CHECK_FALSE(fs::exists(inbox / "capture-20261025_060000-11223344.jpg"));
}

TEST_CASE("helper copy refuses a capture path outside the root") {
    splat_test::TempDir dir("helper-traversal-root");
    splat_test::TempDir outside("helper-traversal-outside");
    const CaptureTree tree{dir.path()};
    const Path inbox = dir.path() / "inbox";

    const Path secret = outside.path() / "20261024_101112_aabbccdd.jpg";
    write_file(secret, std::string(64, 's'));

    // Both spellings of "outside": an absolute sibling and a `..` walk-up.
    const HelperResult result =
        splat::copy_captures(dir.path(), inbox, {secret, dir.path() / ".." / outside.path().filename() / secret.filename()});
    CHECK_FALSE(result.ok);
    CHECK(result.copied.empty());
    CHECK(result.error.find("outside capture root") != std::string::npos);
    CHECK_FALSE(fs::exists(inbox));
}

TEST_CASE("helper copy refuses an HDR jxr capture with an error entry") {
    splat_test::TempDir dir("helper-jxr");
    const CaptureTree tree{dir.path()};
    const Path inbox = dir.path() / "inbox";

    const HelperResult result =
        splat::copy_captures(dir.path(), inbox,
                             {capture_path(dir.path(), "20261025_052805_00338445.jxr")});
    CHECK_FALSE(result.ok);
    CHECK(result.copied.empty());
    CHECK(result.error.find("not supported") != std::string::npos);
    CHECK_FALSE(fs::exists(inbox));

    // Its .dat companion is not a capture at all.
    const HelperResult companion =
        splat::copy_captures(dir.path(), inbox,
                             {capture_path(dir.path(), "20261025_052805_00338445.dat")});
    CHECK_FALSE(companion.ok);
    CHECK(companion.copied.empty());
    CHECK(companion.error.find("not a capture") != std::string::npos);
}

TEST_CASE("helper malformed requests are answered with ok false and an error") {
    splat_test::TempDir dir("helper-malformed");
    const CaptureTree tree{dir.path()};
    const Path inbox = dir.path() / "inbox";

    for (const std::string& text : {std::string("not json"),
                                    std::string("{\"id\":1}"),
                                    std::string("{\"id\":1,\"op\":\"delete\"}"),
                                    std::string("{\"id\":1,\"op\":\"copy\"}"),
                                    std::string("{\"id\":1,\"op\":\"copy\",\"paths\":[7]}"),
                                    std::string("{\"op\":\"list\"}")}) {
        const splat::HelperRequest request = splat::parse_request(text);
        CHECK_FALSE(request.valid());

        const nlohmann::json response = answer(text, dir.path(), inbox);
        CHECK(response["ok"] == false);
        CHECK_FALSE(response["error"].get<std::string>().empty());
        CHECK(response["captures"].empty());
        CHECK(response["copied"].empty());
    }

    // The request id is echoed when it was readable, so the client can match.
    const nlohmann::json response = answer(R"({"id":17,"op":"nope"})", dir.path(), inbox);
    CHECK(response["id"] == 17);
}

TEST_CASE("helper root.txt overrides the capture root") {
    splat_test::TempDir dir("helper-root-override");
    const CaptureTree tree{dir.path()};
    const Path helper_dir = dir.path() / "helper";
    fs::create_directories(helper_dir);

    CHECK(splat::helper_root(helper_dir) == Path(splat::kDefaultCaptureRoot));

    write_file(helper_dir / "root.txt", dir.path().string() + "\n");
    CHECK(splat::helper_root(helper_dir) == dir.path());

    // A relative path in root.txt is ignored: the default stays.
    write_file(helper_dir / "root.txt", "relative/root\n");
    CHECK(splat::helper_root(helper_dir) == Path(splat::kDefaultCaptureRoot));

    // The fabricated tree is reachable through the override, as in the tests'
    // fake helper loop.
    write_file(helper_dir / "root.txt", dir.path().string());
    const nlohmann::json response =
        answer(R"({"id":3,"op":"list"})", splat::helper_root(helper_dir), dir.path() / "inbox");
    CHECK(response["ok"] == true);
    CHECK(response["captures"].size() == 4);
}
