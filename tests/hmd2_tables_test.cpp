// The generated export tables (platform/ps5/hmd2/hmd2_exports.h): every table
// carries every name hmd2.cpp resolves, the firmware set is the one imported
// from PS5VR, and a library's tables are newest-first with distinct anchors.
//
// hmd2.cpp's module_table() returns the first table whose anchor matches, so a
// table copied with one row missing, or two tables of one library sharing an
// anchor, would cost a name at runtime instead of failing here. 10.01 (issue
// #2) relies on this: its console uses the 1000 table only if that table is
// complete, and the coverage test says so without a console.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

// hmd2.cpp defines these three next to the include; the header is data only.
struct export_sig_t {
    const char* name;
    uint32_t offset;
};
struct export_anchor_t {
    uint32_t va;
    const char* text;
};
struct export_table_t {
    int fw;
    const export_sig_t* exports;
    size_t count;
    const export_anchor_t* anchor;
};
#include "../platform/ps5/hmd2/hmd2_exports.h"
#include "../platform/ps5/hmd2/hmd2_funcs.h"

namespace {

// The names hmd2.cpp resolves, expanded from the same X-macro lists it uses
// (hmd2_funcs.h), so a name added there cannot be missing from this test.
#define NAME(ret, name, args) #name,
const std::vector<const char*> kHmd2Names = {HMD2_FUNCS(NAME) HMD2_MIRROR_FUNCS(NAME)};
const std::vector<const char*> kVrTracker2Names = {VRTRACKER2_FUNCS(NAME)};
const std::vector<const char*> kVrHandNames = {VRHAND_FUNCS(NAME)};
#undef NAME

struct Library {
    const char* name;
    const export_table_t* tables;
    size_t count;
    const std::vector<const char*>* names;
    std::vector<int> expected_firmware;   // newest first, the order module_table tries
};

template <size_t N>
Library library(const char* name, const export_table_t (&tables)[N],
                const std::vector<const char*>& names, std::vector<int> expected) {
    return {name, tables, N, &names, std::move(expected)};
}

std::vector<Library> libraries() {
    return {
        library("libSceHmd2", kTables_libSceHmd2, kHmd2Names,
                {1360, 1240, 1160, 1000, 840, 761, 550}),
        library("libSceVrTracker2", kTables_libSceVrTracker2, kVrTracker2Names,
                {1360, 1240, 1160, 1000, 840, 761, 550}),
        library("libSceVrHand", kTables_libSceVrHand, kVrHandNames, {1360, 1240}),
    };
}

const export_sig_t* find_export(const export_table_t& table, const char* name) {
    for (size_t i = 0; i < table.count; i++)
        if (std::strcmp(table.exports[i].name, name) == 0)
            return &table.exports[i];
    return nullptr;
}

bool same_anchor(const export_anchor_t& a, const export_anchor_t& b) {
    return a.va == b.va && std::strcmp(a.text, b.text) == 0;
}

}  // namespace

TEST_CASE("every export table carries every name hmd2 resolves") {
    for (const Library& lib : libraries()) {
        CAPTURE(lib.name);
        REQUIRE(lib.count > 0);
        for (size_t t = 0; t < lib.count; t++) {
            const export_table_t& table = lib.tables[t];
            CAPTURE(table.fw);
            REQUIRE(table.anchor != nullptr);
            REQUIRE(table.anchor->text != nullptr);
            CHECK(std::strlen(table.anchor->text) > 0);
            REQUIRE(table.exports != nullptr);
            REQUIRE(table.count > 0);
            for (const char* name : *lib.names) {
                INFO(name);
                CHECK(find_export(table, name) != nullptr);
            }
            for (size_t i = 0; i < table.count; i++)
                for (size_t j = i + 1; j < table.count; j++) {
                    INFO(table.exports[i].name);
                    CHECK(std::strcmp(table.exports[i].name, table.exports[j].name) != 0);
                }
        }
    }
}

TEST_CASE("every table carries the exports only at nonzero offsets") {
    for (const Library& lib : libraries())
        for (size_t t = 0; t < lib.count; t++)
            for (size_t i = 0; i < lib.tables[t].count; i++) {
                CAPTURE(lib.name);
                CAPTURE(lib.tables[t].fw);
                INFO(lib.tables[t].exports[i].name);
                CHECK(lib.tables[t].exports[i].offset != 0);
            }
}

TEST_CASE("the imported firmware set is PS5VR's, newest first") {
    for (const Library& lib : libraries()) {
        CAPTURE(lib.name);
        std::vector<int> fws;
        for (size_t t = 0; t < lib.count; t++)
            fws.push_back(lib.tables[t].fw);
        CHECK(fws == lib.expected_firmware);   // also: descending, so newest is tried first
        for (size_t i = 1; i < fws.size(); i++)
            CHECK(fws[i - 1] > fws[i]);
    }
}

TEST_CASE("10.01 is served by the 10.00 table") {
    // 10.01 has no table of its own: PS5VR generated 1000 from the 10.00 build
    // and checked 10.40/10.60 against it, so a 10.01 console's library either
    // matches that anchor or fails closed with "no export table".
    for (const Library& lib : libraries()) {
        if (std::strcmp(lib.name, "libSceVrHand") == 0)
            continue;   // its 10.x API differs, so PS5VR has no VrHand table there
        CAPTURE(lib.name);
        bool found = false;
        for (size_t t = 0; t < lib.count; t++)
            found = found || lib.tables[t].fw == 1000;
        CHECK(found);
    }
}

TEST_CASE("a library's tables do not share an anchor") {
    // module_table() returns the first match, so an anchor repeated at the same
    // address would hide the later table.
    for (const Library& lib : libraries()) {
        CAPTURE(lib.name);
        for (size_t i = 0; i < lib.count; i++)
            for (size_t j = i + 1; j < lib.count; j++) {
                CAPTURE(lib.tables[i].fw);
                CAPTURE(lib.tables[j].fw);
                CHECK(!same_anchor(*lib.tables[i].anchor, *lib.tables[j].anchor));
            }
    }
}
