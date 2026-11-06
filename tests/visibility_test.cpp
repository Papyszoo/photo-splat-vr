// The visible-folders state file and the key/placement rules: what "no
// passcode" means, what a damaged file means (nothing visible), the 0600 hash
// file, the scene-folder keys, and the fact that a photo path is only ever a
// provenance key now (stage U1: the picker is not filtered).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/passcode.h"
#include "common/path.h"
#include "gallery/visibility.h"
#include "test_util.h"

namespace {

using splat::Path;
using splat::Visibility;
using splat::VisibilityContext;
using splat::VisibilityKey;
using splat::VisibilityKind;

struct Fixture {
    splat_test::TempDir dir{"visible"};
    Path base = dir.path();
    Path data = base / "data";
    Path app = data / "ps5-splat-vr";
    VisibilityContext context;
    Path state = base / "state";

    Fixture() {
        context.scenes_root = app / "scenes";
        context.inbox = app / "inbox";
        context.mnt = base / "mnt";
        context.data = data;
        context.app_data = app;
        std::error_code error;
        std::filesystem::create_directories(state, error);
    }

    Visibility load() const { return splat::load_visibility(state, context).value(); }

    void write_text(const std::string& text) const {
        std::ofstream out(splat::visible_folders_path(state), std::ios::binary | std::ios::trunc);
        out << text;
    }
};

std::string damage_json() { return "{\"format_version\": 1, \"passcode\": "; }

std::vector<std::string> texts(const Visibility& state) {
    std::vector<std::string> out;
    for (const VisibilityKey& key : state.allowed) out.push_back(key.text());
    return out;
}

}  // namespace

TEST_CASE("visibility without a state file shows every folder") {
    Fixture fixture;
    const Visibility state = fixture.load();
    CHECK_FALSE(state.enabled);
    CHECK_FALSE(state.damaged);
    CHECK(state.has_context());
    CHECK(state.allows_album("Family A"));
    CHECK(state.allows_unsorted());
    CHECK(state.allows_location("smb0"));
    CHECK(state.allows(VisibilityKey::other()));
    CHECK(state.allows_scene(fixture.context.scenes_root / "Family A" / "scene-a",
                             VisibilityKey::album("Family A")));
    // A host always has a policy object; stage H's install makes it this one.
    CHECK(splat::current_visibility() != nullptr);
}

TEST_CASE("visibility state file round-trips and stores a hash, never the passcode") {
    Fixture fixture;
    Visibility state = Visibility::all_visible(fixture.context);
    REQUIRE(state.enable(fixture.state, "2468").ok());
    CHECK(state.check_passcode("2468"));
    REQUIRE(state.allows_album("Family A") == false);
    state.allowed.push_back(VisibilityKey::album("Family A"));
    state.allowed.push_back(VisibilityKey::photo_source("inbox"));
    std::sort(state.allowed.begin(), state.allowed.end());
    REQUIRE(state.save_allowed(fixture.state).ok());

    const Path file = splat::visible_folders_path(fixture.state);
    REQUIRE(std::filesystem::exists(file));
    std::ifstream in(file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.find("2468") == std::string::npos);
    const nlohmann::json root = nlohmann::json::parse(text);
    CHECK(root.at("format_version").get<int>() == 1);
    CHECK(root.at("passcode").at("algorithm").get<std::string>() == "pbkdf2-hmac-sha256");
    CHECK(root.at("passcode").at("iterations").get<int>() == 120000);
    CHECK(root.at("passcode").at("salt").get<std::string>().size() == 32);
    CHECK(root.at("passcode").at("hash").get<std::string>().size() == 64);
    CHECK(root.at("visible").is_array());

    const Visibility reloaded = fixture.load();
    REQUIRE(reloaded.enabled);
    CHECK_FALSE(reloaded.damaged);
    CHECK(reloaded.check_passcode("2468"));
    CHECK_FALSE(reloaded.check_passcode("2469"));
    CHECK(texts(reloaded) == (std::vector<std::string>{"album:Family A", "source:inbox"}));
    CHECK(reloaded.allows_album("Family A"));
    CHECK(reloaded.allows_unsorted() == false);
}

TEST_CASE("visibility file is 0600 on POSIX") {
#if !defined(_WIN32)
    Fixture fixture;
    Visibility state = Visibility::all_visible(fixture.context);
    REQUIRE(state.enable(fixture.state, "2468").ok());
    const auto status = std::filesystem::status(splat::visible_folders_path(fixture.state));
    const auto expected = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
    CHECK(status.permissions() == expected);
#endif
}

TEST_CASE("visibility two consoles with the same passcode store different salts") {
    Fixture first;
    Fixture second;
    Visibility a = Visibility::all_visible(first.context);
    Visibility b = Visibility::all_visible(second.context);
    REQUIRE(a.enable(first.state, "2468").ok());
    REQUIRE(b.enable(second.state, "2468").ok());
    CHECK(a.passcode.salt != b.passcode.salt);
    CHECK(a.passcode.hash != b.passcode.hash);
}

TEST_CASE("visibility refuses a passcode outside four to thirty-two characters") {
    Fixture fixture;
    Visibility state = Visibility::all_visible(fixture.context);
    CHECK_FALSE(state.enable(fixture.state, "123").ok());
    CHECK_FALSE(state.enable(fixture.state, std::string(33, 'a')).ok());
    CHECK_FALSE(std::filesystem::exists(splat::visible_folders_path(fixture.state)));
    CHECK(state.enable(fixture.state, "1234").ok());
    CHECK_FALSE(state.change_passcode(fixture.state, "1234", "12").ok());
}

TEST_CASE("visibility damaged files fail closed") {
    // Every one of these is a file that exists and cannot be trusted: the answer
    // is "nothing visible", never "no passcode".
    const std::vector<std::string> broken = {
        "{not json",
        "[]",
        R"({"format_version": 1})",
        R"({"format_version": 2, "passcode": {"algorithm": "pbkdf2-hmac-sha256",
            "iterations": 120000, "salt": "00112233445566778899aabbccddeeff",
            "hash": "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"},
            "visible": []})",
        R"({"format_version": 1, "passcode": {"algorithm": "sha1", "iterations": 120000,
            "salt": "00112233445566778899aabbccddeeff",
            "hash": "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"},
            "visible": []})",
        R"({"format_version": 1, "passcode": {"algorithm": "pbkdf2-hmac-sha256",
            "iterations": 1, "salt": "00112233445566778899aabbccddeeff",
            "hash": "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"},
            "visible": []})",
        R"({"format_version": 1, "passcode": {"algorithm": "pbkdf2-hmac-sha256",
            "iterations": 120000, "salt": "abc",
            "hash": "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"},
            "visible": []})",
        R"({"format_version": 1, "passcode": {"algorithm": "pbkdf2-hmac-sha256",
            "iterations": 120000, "salt": "00112233445566778899aabbccddeeff",
            "hash": "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"},
            "visible": ["album:Family A", "shelf:whatever"]})",
        damage_json() + "null}",
    };
    for (const std::string& text : broken) {
        Fixture fixture;
        fixture.write_text(text);
        const Visibility state = fixture.load();
        CHECK(state.enabled);
        CHECK(state.damaged);
        CHECK(state.allowed.empty());
        CHECK_FALSE(state.check_passcode(""));
        CHECK_FALSE(state.allows_album("Family A"));
        CHECK_FALSE(state.allows_unsorted());
        CHECK_FALSE(state.allows_location("smb0"));
        CHECK_FALSE(state.allows(VisibilityKey::other()));
        CHECK_FALSE(state.allows_scene(fixture.context.scenes_root / "Family A" / "scene-a",
                                       VisibilityKey::album("Family A")));
        // The screen says the file is damaged, and only a PC delete recovers it.
    }
}

TEST_CASE("visibility disable removes the file and change keeps the list") {
    Fixture fixture;
    Visibility state = Visibility::all_visible(fixture.context);
    REQUIRE(state.enable(fixture.state, "2468").ok());
    state.allowed.push_back(VisibilityKey::album("Family A"));
    REQUIRE(state.save_allowed(fixture.state).ok());

    REQUIRE(state.change_passcode(fixture.state, "2468", "1357").ok());
    CHECK(state.check_passcode("1357"));
    CHECK_FALSE(state.check_passcode("2468"));
    CHECK(state.allows_album("Family A"));  // the allow-list is untouched
    const std::string before = state.passcode.salt;
    REQUIRE(state.change_passcode(fixture.state, "1357", "1357").ok());
    CHECK(state.passcode.salt != before);  // a new salt for the same passcode

    const Visibility reloaded = fixture.load();
    CHECK(reloaded.check_passcode("1357"));
    CHECK(reloaded.allows_album("Family A"));

    REQUIRE(state.disable(fixture.state).ok());
    CHECK_FALSE(std::filesystem::exists(splat::visible_folders_path(fixture.state)));
    CHECK_FALSE(state.enabled);
    CHECK_FALSE(state.damaged);
    CHECK(state.allowed.empty());
    CHECK(state.allows_album("Family A"));
    CHECK_FALSE(fixture.load().enabled);  // a relaunch has no passcode
}

TEST_CASE("visibility scope restores the previous policy") {
    Fixture fixture;
    Visibility first = Visibility::all_visible(fixture.context);
    first.enabled = true;
    first.allowed.push_back(VisibilityKey::album("Family A"));
    splat::VisibilityScope outer(first);
    CHECK_FALSE(splat::current_visibility()->allows_album("Family B"));

    {
        Visibility second = Visibility::all_visible(fixture.context);
        second.enabled = true;
        second.allowed.push_back(VisibilityKey::album("Family B"));
        splat::VisibilityScope scope(second);
        CHECK(splat::current_visibility()->allows_album("Family B"));
        CHECK_FALSE(splat::current_visibility()->allows_album("Family A"));
    }
    CHECK(splat::current_visibility()->allows_album("Family A"));
    CHECK_FALSE(splat::current_visibility()->allows_album("Family B"));
}

TEST_CASE("visibility keys parse and print for all six kinds") {
    struct Case {
        VisibilityKey key;
        const char* text;
    };
    const Case cases[] = {
        {VisibilityKey::album("Family A"), "album:Family A"},
        {VisibilityKey::unsorted(), "unsorted"},
        {VisibilityKey::other(), "other"},
        {VisibilityKey::photo_source("usb0"), "source:usb0"},
        {VisibilityKey::console_folder(Path("/data/Photos")), "folder:/data/Photos"},
        {VisibilityKey::location("smb0"), "location:smb0"},
    };
    for (const Case& one : cases) {
        CHECK(one.key.text() == one.text);
        const splat::Result<VisibilityKey> parsed = VisibilityKey::parse(one.text);
        REQUIRE(parsed.ok());
        CHECK(parsed.value() == one.key);
        CHECK(parsed.value().text() == one.text);
    }
    CHECK(VisibilityKey::parse("").ok() == false);
    CHECK(VisibilityKey::parse("album:").ok() == false);
    CHECK(VisibilityKey::parse("album:Family A").value().id == "Family A");
    CHECK(VisibilityKey::parse("folder:relative/path").ok() == false);
    CHECK(VisibilityKey::parse("shelf:x").ok() == false);
    CHECK(VisibilityKey::parse("other ").ok() == false);
}

TEST_CASE("visibility maps a photo or scene path to its folder") {
    Fixture fixture;
    Visibility state = Visibility::all_visible(fixture.context);
    state.enabled = true;
    const auto key = [&](const Path& path) { return state.key_for_source(path).text(); };

    CHECK(key(fixture.context.inbox / "a.jpg") == "source:inbox");
    CHECK(key(fixture.context.mnt / "usb0" / "a.jpg") == "source:usb0");
    CHECK(key(fixture.context.mnt / "ext0" / "Photos" / "a.jpg") ==
          "folder:" + (fixture.context.mnt / "ext0" / "Photos").generic_string());
    CHECK(key(fixture.context.data / "Family" / "a.jpg") ==
          "folder:" + (fixture.context.data / "Family").generic_string());
    CHECK(key(fixture.context.app_data / "state" / "x") == "other");
    CHECK(key(Path("smb0:/a")) == "location:smb0");
    CHECK(key(fixture.context.scenes_root / "Family" / "scene-a") == "album:Family");
    CHECK(key(fixture.context.scenes_root / "loose") == "unsorted");
    CHECK(key(Path("/tmp/elsewhere/x.jpg")) == "other");
    // A file directly in a storage root has no tickable top-level folder.
    CHECK(key(fixture.context.data / "a.jpg") == "other");
}

TEST_CASE("visibility a scene follows its placement only") {
    // Stage U1: the source half is gone. A scene converted from a photo folder
    // whose key is not (and can no longer be) ticked is visible under a ticked
    // album; an unticked album still hides it; a damaged state file hides all.
    Fixture fixture;
    const Path scene = fixture.context.scenes_root / "Family A" / "from-inbox";
    Visibility state = Visibility::all_visible(fixture.context);
    state.enabled = true;
    state.allowed = {VisibilityKey::album("Family A")};

    // The scene's meta.json says source:inbox (an unticked photo key). It is
    // no longer read: the placement decides.
    CHECK(state.allows_scene(scene, VisibilityKey::album("Family A")));
    CHECK(state.allows_scene_folder(scene));
    CHECK(state.allows_scene_path(scene / "scene.psplat"));

    // The same scene under an unticked album stays hidden, whatever its source.
    CHECK_FALSE(state.allows_scene(scene, VisibilityKey::album("Family B")));
    CHECK_FALSE(state.allows_scene_folder(fixture.context.scenes_root / "Family B" / "scene-b"));

    // Unsorted is a placement of its own: a scene directly under the library is
    // listed as Unsorted, and a scene two levels down has no row ("other").
    state.allowed = {VisibilityKey::unsorted()};
    CHECK(state.allows_scene_folder(fixture.context.scenes_root / "loose-scene"));
    CHECK_FALSE(state.allows_scene_folder(fixture.context.scenes_root / "loose" / "deeper"));

    // A damaged state file still hides every scene and every row.
    fixture.write_text(damage_json() + "null}");
    const Visibility damaged = fixture.load();
    REQUIRE(damaged.damaged);
    CHECK_FALSE(damaged.allows_scene(scene, VisibilityKey::album("Family A")));
    CHECK_FALSE(damaged.allows_scene_folder(scene));
    CHECK_FALSE(damaged.allows_scene_path(scene / "scene.psplat"));
}

TEST_CASE("visibility old photo keys still load and hide nothing") {
    // A state file written by stage H can hold source:/folder: keys. It is not
    // a damaged file (the owner would have to delete it to recover); the keys
    // are inert and no photo folder is hidden by this list any more.
    Fixture fixture;
    Visibility state = Visibility::all_visible(fixture.context);
    REQUIRE(state.enable(fixture.state, "2468").ok());
    state.allowed = {VisibilityKey::album("Family A"),
                     VisibilityKey::photo_source("inbox"),
                     VisibilityKey::console_folder(fixture.context.data / "Photos")};
    std::sort(state.allowed.begin(), state.allowed.end());
    REQUIRE(state.save_allowed(fixture.state).ok());

    const Visibility reloaded = fixture.load();
    CHECK(reloaded.enabled);
    CHECK_FALSE(reloaded.damaged);
    CHECK(reloaded.allowed.size() == 3);
    CHECK(reloaded.allows_album("Family A"));
    CHECK_FALSE(reloaded.allows_album("Family B"));
    // The photo keys parse, so the file is readable; they reveal nothing.
    CHECK(reloaded.allows_text("source:inbox"));
    CHECK_FALSE(reloaded.allows_text("source:usb0"));
}

TEST_CASE("visibility no longer judges a top-level photo folder") {
    // Stage U1 removed the console-folder row and every photo rule. The
    // mapping still names the folder (it is written into meta.json), but a
    // tick on it decides nothing: no album, no scene and no other row.
    Fixture fixture;
    Visibility state = Visibility::all_visible(fixture.context);
    state.enabled = true;
    state.allowed.push_back(VisibilityKey::console_folder(fixture.context.data / "Photos"));
    std::sort(state.allowed.begin(), state.allowed.end());

    const Path root = fixture.context.data;
    CHECK(state.key_for_source(root / "Photos" / "2026" / "a.jpg").text() ==
          "folder:" + (root / "Photos").generic_string());
    // The photo half's failures are gone with it: an unticked photo folder or a
    // file directly in a root is not "not visible" any more, it is simply not
    // this list's business.
    CHECK_FALSE(state.allows_album("Photos"));
    CHECK_FALSE(state.allows_unsorted());
    CHECK_FALSE(state.allows(VisibilityKey::other()));
}
