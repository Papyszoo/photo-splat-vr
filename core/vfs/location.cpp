// SPDX-License-Identifier: GPL-3.0-or-later

#include "vfs/location.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "vfs/nfs.h"
#include "vfs/smb.h"

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace splat::vfs {
namespace {

constexpr int kLocationFormatVersion = 1;


std::string protocol_token(LocationProtocol protocol) {
    return protocol == LocationProtocol::nfs ? std::string("nfs") : std::string("smb");
}

bool parse_protocol(const std::string& token, LocationProtocol* out) {
    if (token == "smb") {
        *out = LocationProtocol::smb;
        return true;
    }
    if (token == "nfs") {
        *out = LocationProtocol::nfs;
        return true;
    }
    return false;
}

// "smb0" -> ("smb", 0); anything else -> false. The id is the location path's
// prefix, so it must stay inside the VFS grammar (vfs::valid_location_id).
bool parse_id(const std::string& id, std::string* prefix, int* number) {
    if (!valid_location_id(id)) return false;
    std::size_t digits = id.size();
    while (digits > 0 && id[digits - 1] >= '0' && id[digits - 1] <= '9') --digits;
    if (digits == 0 || digits == id.size()) return false;
    const std::string head = id.substr(0, digits);
    if (head != "smb" && head != "nfs") return false;
    int value = 0;
    for (std::size_t i = digits; i < id.size(); ++i) {
        value = value * 10 + (id[i] - '0');
        if (value > 9999) return false;
    }
    *prefix = head;
    *number = value;
    return true;
}

std::string id_for(const std::string& prefix, int number) {
    return prefix + std::to_string(number);
}

}  // namespace

const char* location_protocol_name(LocationProtocol protocol) {
    switch (protocol) {
        case LocationProtocol::smb: return "SMB";
        case LocationProtocol::nfs: return "NFS";
    }
    return "SMB";
}

std::string Location::display_name() const {
    if (!name.empty()) return name;
    if (share.empty()) return host;
    return host.empty() ? share : host + "/" + share;
}

int Location::effective_port() const {
    if (port > 0) return port;
    return protocol == LocationProtocol::nfs ? 2049 : 445;
}

Result<std::vector<Location>> locations_from_json(const std::string& json) {
    std::vector<Location> locations;
    try {
        const nlohmann::json root = nlohmann::json::parse(json);
        if (!root.is_object()) {
            return Result<std::vector<Location>>::failure("locations.json root is not an object");
        }
        if (!root.contains("locations")) {
            return Result<std::vector<Location>>::failure("locations.json has no \"locations\"");
        }
        if (!root.at("locations").is_array()) {
            return Result<std::vector<Location>>::failure(
                "locations.json \"locations\" is not an array");
        }
        // The entry count decides the vector's allocation and every string
        // parsed below, so it is validated before the first Location is built.
        const std::size_t entries = root.at("locations").size();
        if (entries > LocationStore::kMaxEntries) {
            return Result<std::vector<Location>>::failure(
                "locations.json holds " + std::to_string(entries) + " entries, more than the " +
                std::to_string(LocationStore::kMaxEntries) + " entry limit");
        }
        for (const nlohmann::json& entry : root.at("locations")) {
            Location location;
            location.id = entry.at("id").get<std::string>();
            if (!valid_location_id(location.id)) {
                return Result<std::vector<Location>>::failure(
                    "locations.json has an invalid id \"" + location.id + "\"");
            }
            const std::string protocol = entry.value("protocol", std::string("smb"));
            if (!parse_protocol(protocol, &location.protocol)) {
                return Result<std::vector<Location>>::failure(
                    "locations.json has an unknown protocol \"" + protocol + "\"");
            }
            location.name = entry.value("name", std::string());
            location.host = entry.value("host", std::string());
            location.port = entry.value("port", 0);
            location.share = entry.value("share", std::string());
            location.path = entry.value("path", std::string());
            location.user = entry.value("user", std::string());
            location.password = entry.value("password", std::string());
            location.domain = entry.value("domain", std::string("WORKGROUP"));
            location.encrypt = entry.value("encrypt", false);
            location.photo_source = entry.value("photo_source", false);
            location.splat_library = entry.value("splat_library", false);
            location.nfs_version = entry.value("nfs_version", 3);
            location.uid = entry.value("uid", 65534u);
            location.gid = entry.value("gid", 65534u);
            locations.push_back(std::move(location));
        }
    } catch (const std::exception& error) {
        return Result<std::vector<Location>>::failure(std::string("invalid locations.json: ") +
                                                      error.what());
    }
    return Result<std::vector<Location>>::success(std::move(locations));
}

std::string locations_to_json(const std::vector<Location>& locations) {
    nlohmann::json root;
    root["version"] = kLocationFormatVersion;
    root["locations"] = nlohmann::json::array();
    for (const Location& location : locations) {
        nlohmann::json entry;
        entry["id"] = location.id;
        entry["name"] = location.name;
        entry["protocol"] = protocol_token(location.protocol);
        entry["host"] = location.host;
        entry["port"] = location.port;
        entry["share"] = location.share;
        entry["path"] = location.path;
        entry["user"] = location.user;
        // Plain text by design: the screen warns and docs/network-locations.md
        // says why (no key store on the console; an encrypted file would need a
        // key that would live next to it).
        entry["password"] = location.password;
        entry["domain"] = location.domain;
        entry["encrypt"] = location.encrypt;
        entry["photo_source"] = location.photo_source;
        entry["splat_library"] = location.splat_library;
        entry["nfs_version"] = location.nfs_version;
        entry["uid"] = location.uid;
        entry["gid"] = location.gid;
        root["locations"].push_back(std::move(entry));
    }
    return root.dump(2) + "\n";
}

Result<LocationStore> LocationStore::open(const Path& file) {
    LocationStore store;
    store.file_ = file;

    FileStat status;
    if (!stat(file, &status).ok()) {
        return Result<LocationStore>::failure("cannot inspect " + file.string());
    }
    if (!status.exists) return Result<LocationStore>::success(std::move(store));

    // Checked from the stat result, before the bytes are read: this file is on
    // the boot path, and a foreign one must not decide how much is allocated.
    if (status.size > LocationStore::kMaxFileBytes) {
        return Result<LocationStore>::failure(
            "locations.json " + file.string() + " is " + std::to_string(status.size) +
            " bytes, more than the " + std::to_string(LocationStore::kMaxFileBytes) +
            " byte limit; not a store this build wrote");
    }

    const Result<std::vector<uint8_t>> bytes = read_file(file, LocationStore::kMaxFileBytes);
    if (!bytes.ok()) return Result<LocationStore>::failure(bytes.message());
    const std::string json(bytes.value().begin(), bytes.value().end());
    Result<std::vector<Location>> parsed = locations_from_json(json);
    if (!parsed.ok()) {
        return Result<LocationStore>::failure(parsed.message() + " (" + file.string() + ")");
    }
    store.locations_ = parsed.take();
    store.recount_ids();
    return Result<LocationStore>::success(std::move(store));
}

const Location* LocationStore::find(const std::string& id) const {
    for (const Location& location : locations_) {
        if (location.id == id) return &location;
    }
    return nullptr;
}

void LocationStore::recount_ids() {
    next_smb_ = 0;
    next_nfs_ = 0;
    for (const Location& location : locations_) {
        std::string prefix;
        int number = 0;
        if (!parse_id(location.id, &prefix, &number)) continue;
        if (prefix == "smb") {
            next_smb_ = std::max(next_smb_, number + 1);
        } else {
            next_nfs_ = std::max(next_nfs_, number + 1);
        }
    }
}

Result<std::string> LocationStore::add(Location location) {
    if (location.host.empty()) return Result<std::string>::failure("a location needs a host");
    if (location.share.empty()) return Result<std::string>::failure("a location needs a share");

    // The id follows the protocol's highest number ever handed out in this
    // file, not the list length: removing smb0 then adding another location
    // gives smb1, so a saved path never points at a different share.
    const std::string prefix = protocol_token(location.protocol);
    const int number = prefix == "smb" ? next_smb_ : next_nfs_;
    location.id = id_for(prefix, number);
    (prefix == "smb" ? next_smb_ : next_nfs_) = number + 1;

    locations_.push_back(std::move(location));
    const Status saved = save();
    if (!saved.ok()) {
        locations_.pop_back();
        (prefix == "smb" ? next_smb_ : next_nfs_) = number;
        return Result<std::string>::failure(saved.message());
    }
    return Result<std::string>::success(locations_.back().id);
}

Status LocationStore::update(const Location& location) {
    for (Location& existing : locations_) {
        if (existing.id != location.id) continue;
        const Location previous = existing;
        existing = location;
        const Status saved = save();
        if (!saved.ok()) {
            existing = previous;
            return saved;
        }
        return Status::success();
    }
    return Status::error("no such location: " + location.id);
}

Status LocationStore::remove(const std::string& id) {
    const auto found = std::find_if(locations_.begin(), locations_.end(),
                                    [&id](const Location& location) { return location.id == id; });
    if (found == locations_.end()) return Status::error("no such location: " + id);
    locations_.erase(found);
    return save();
}

Status LocationStore::save() const {
    const std::string json = locations_to_json(locations_);
    const Path parent = file_.parent_path();
    if (!parent.empty()) {
        const Status made = make_dirs(parent);
        if (!made.ok()) return made;
    }

    std::FILE* file = std::fopen(file_.string().c_str(), "wb");
    if (file == nullptr) return Status::error("cannot open " + file_.string() + " for writing");
#if !defined(_WIN32)
    // The file holds a plain-text password: only the owner may read it.
    (void)::chmod(file_.string().c_str(), 0600);
#endif
    const std::size_t written = std::fwrite(json.data(), 1, json.size(), file);
    const bool good = written == json.size();
    if (std::fclose(file) != 0 || !good) {
        return Status::error("failed while writing " + file_.string());
    }
    return Status::success();
}

std::string LocationStore::mount_one(FileSystems& file_systems, const Location& location) {
    // The adapter may already exist from register_adapters() (a host registers
    // every configured location before the queue runs). Probing that one keeps
    // one context per location; a second adapter would answer from its own
    // connection and the two could disagree about "ready".
    std::shared_ptr<IFileSystem> mounted = file_systems.find(location.id);
    if (mounted == nullptr) {
        std::string error;
        mounted = location.protocol == LocationProtocol::nfs
                      ? make_nfs_file_system(location, &error)
                      : make_smb_file_system(location, &error);
        if (!mounted) {
            return "offline: " + (error.empty() ? std::string("cannot connect") : error);
        }
        // Registered before the probe: the probe is the part that can take 5 s,
        // and a caller must never see "unknown location" for a configured one.
        file_systems.add(mounted);
    }

    // The mount is where the user is told the truth: one root stat forces the
    // lazy connect, so "ready" means the share answered. A root that answers
    // "no such file" is not ready either: the export exists but the location's
    // Path below it does not, and every listing under it would fail.
    //
    // The adapter is reset first: a location that failed once must be able to
    // succeed on this attempt - a NAS that was still booting when the app
    // started, or the button the user pressed after fixing the address
    // (console pass 3, L1). A cached "cannot connect" would otherwise answer
    // this mount instead of the server.
    mounted->reset_connection();
    FileStat root;
    const Status probed = mounted->stat("", &root);
    if (!probed.ok()) return "offline: " + probed.message();
    if (!root.exists) {
        return "offline: " + location.id + ": " +
               (location.path.empty() ? std::string("the share root")
                                      : "\"" + location.path + "\"") +
               " does not exist on " + location.host;
    }
    return "ready";
}

std::vector<std::string> LocationStore::register_adapters(FileSystems& file_systems) const {
    std::vector<std::string> failed;
    for (const Location& location : locations_) {
        if (file_systems.find(location.id) != nullptr) continue;  // already registered
        std::string error;
        std::shared_ptr<IFileSystem> mounted =
            location.protocol == LocationProtocol::nfs ? make_nfs_file_system(location, &error)
                                                       : make_smb_file_system(location, &error);
        if (!mounted) {
            failed.push_back(location.id + ": " +
                             (error.empty() ? std::string("cannot create the adapter") : error));
            continue;
        }
        file_systems.add(std::move(mounted));
    }
    return failed;
}

std::vector<std::string> LocationStore::mount_all(FileSystems& file_systems) {
    std::vector<std::string> lines;
    lines.reserve(locations_.size());
    for (const Location& location : locations_) {
        std::string state = mount_one(file_systems, location);
        // mount_one()'s reason already names the location ("offline: smb0:
        // cannot connect ...", the text the Locations screen shows under the
        // row's own id), so adding the id here again read "smb0: offline:
        // smb0: ...". Insert the id once, before the state.
        const std::string inside = "offline: " + location.id + ": ";
        if (state.compare(0, inside.size(), inside) == 0) {
            state = "offline: " + state.substr(inside.size());
        }
        lines.push_back(location.id + ": " + state);
    }
    return lines;
}

}  // namespace splat::vfs
