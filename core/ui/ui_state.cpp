// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/ui_state.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "gallery/gallery.h"
#include "ui/services.h"
#include "vfs/io.h"

namespace splat {
namespace {

constexpr uint32_t kStateFormatVersion = 1;
// A real file is a handful of bytes; a larger one is not ours.
constexpr std::streamsize kMaxStateBytes = 64 * 1024;

bool read_text(const Path& path, std::string* text) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    text->assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return !file.bad();
}

// The components of a location path's relative part, empty ones dropped. False
// for "." and "..": the browser never produces them, and a stored one would
// climb out of the folder the user chose.
bool relative_components(const std::string& relative, std::vector<std::string>* out) {
    out->clear();
    std::size_t start = 0;
    while (start <= relative.size()) {
        const std::size_t slash = relative.find('/', start);
        const std::string part = relative.substr(
            start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part == "." || part == "..") return false;
        if (!part.empty()) out->push_back(part);
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

bool plain_text(const std::string& text) {
    for (const char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f || c == '\\') return false;
    }
    return true;
}

}  // namespace

Path ui_state_path(const Path& state_dir) { return state_dir / "ui.json"; }

bool valid_output_folder(const std::string& text) {
    if (text.empty() || text.size() > kMaxOutputFolderChars || !plain_text(text)) return false;
    std::string relative;
    if (!vfs::split(Path(text), nullptr, &relative)) return false;
    std::vector<std::string> parts;
    if (!relative_components(relative, &parts)) return false;
    // Canonical only: one spelling per folder, so two screens compare by text.
    std::string canonical;
    for (const std::string& part : parts) canonical += (canonical.empty() ? "" : "/") + part;
    return canonical == relative;
}

std::string canonical_output_folder(const Path& folder) {
    std::string id;
    std::string relative;
    if (!vfs::split(folder, &id, &relative) || !plain_text(folder.string())) return std::string();
    std::vector<std::string> parts;
    if (!relative_components(relative, &parts)) return std::string();
    std::string joined;
    for (const std::string& part : parts) joined += (joined.empty() ? "" : "/") + part;
    const std::string text = vfs::join(id, joined).string();
    return valid_output_folder(text) ? text : std::string();
}

namespace {

const LocationView* find_location(const ILocationService* locations, const std::string& id,
                                  std::vector<LocationView>* storage) {
    if (locations == nullptr) return nullptr;
    *storage = locations->list();
    for (const LocationView& view : *storage) {
        if (view.id == id) return &view;
    }
    return nullptr;
}

}  // namespace

std::string network_folder_text(const ILocationService* locations, const Path& folder) {
    std::string id;
    std::string relative;
    if (!vfs::split(folder, &id, &relative)) return folder.string();
    std::vector<LocationView> storage;
    const LocationView* location = find_location(locations, id, &storage);
    if (location == nullptr) return id + " (removed)";
    std::string text = location->name.empty() ? location->id : location->name;
    std::vector<std::string> parts;
    relative_components(relative, &parts);
    for (const std::string& part : parts) text += " \xE2\x80\xBA " + part;
    return text;
}

std::string output_folder_text(const UiState* state, const ILocationService* locations) {
    if (state == nullptr || state->output_folder.empty()) return "Console";
    return network_folder_text(locations, Path(state->output_folder));
}

Result<SceneTarget> scene_target(const UiState* state, const ILocationService* locations,
                                 const Path& shelf_root, const std::string& shelf_album) {
    SceneTarget target;
    if (vfs::is_remote(shelf_root)) {
        target.root = shelf_root;
        target.album = shelf_album;
        return Result<SceneTarget>::success(std::move(target));
    }
    if (state == nullptr || state->output_folder.empty()) {
        target.album = shelf_album;
        return Result<SceneTarget>::success(std::move(target));
    }
    const Path folder(state->output_folder);
    std::vector<LocationView> storage;
    if (find_location(locations, vfs::location_id(folder), &storage) == nullptr) {
        return Result<SceneTarget>::failure(
            "The output folder's network location was removed. Choose another in Settings.");
    }
    // The shelf's folder goes along as `<output folder>/<album>`, the console's
    // own album rules deciding which names are usable: a name that is no folder
    // name on the share is refused, never dropped or rewritten.
    if (!shelf_album.empty() && !valid_album_name(shelf_album)) {
        return Result<SceneTarget>::failure(
            "This folder's name cannot be used on a network share. Rename the folder first.");
    }
    target.root = folder;
    target.album = shelf_album;
    target.from_setting = true;
    return Result<SceneTarget>::success(std::move(target));
}

UiState load_ui_state(const Path& state_dir) {
    UiState state;
    std::error_code error;
    const Path path = ui_state_path(state_dir);
    if (!std::filesystem::is_regular_file(path, error)) return state;
    const uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size > static_cast<uintmax_t>(kMaxStateBytes)) return state;

    std::string text;
    if (!read_text(path, &text)) return state;

    try {
        const nlohmann::json root = nlohmann::json::parse(text);
        if (!root.is_object()) return state;
        const auto selected = root.find("selected_model");
        if (selected != root.end() && selected->is_string()) {
            state.selected_model = selected->get<std::string>();
        }
        const auto album = root.find("last_album");
        if (album != root.end() && album->is_string()) {
            state.last_album = album->get<std::string>();
        }
        const auto tooltips = root.find("tooltips");
        if (tooltips != root.end() && tooltips->is_boolean()) {
            state.tooltips = tooltips->get<bool>();
        }
        const auto scale = root.find("scale_around");
        if (scale != root.end()) {
            if (scale->is_number_integer()) {
                const int value = scale->get<int>();
                state.scale_around = value == 0 ? 0 : (value == 1 ? 1 : 2);
            } else if (scale->is_string()) {
                const std::string value = scale->get<std::string>();
                state.scale_around = value == "front" ? 0 : (value == "centre" ? 1 : 2);
            }
        }
        const auto move = root.find("move_speed");
        if (move != root.end() && move->is_number()) {
            state.move_speed =
                kMoveSpeeds[nearest_move_speed_index(static_cast<float>(move->get<double>()))];
        }
        const auto turn = root.find("turn_speed");
        if (turn != root.end() && turn->is_number()) {
            state.turn_speed =
                kTurnSpeeds[nearest_turn_speed_index(static_cast<float>(turn->get<double>()))];
        }
        const auto edge = root.find("edge_softness");
        if (edge != root.end() && edge->is_number()) {
            state.edge_softness = kEdgeSoftness[nearest_edge_softness_index(
                static_cast<float>(edge->get<double>()))];
        }
        const auto blend = root.find("blending");
        if (blend != root.end()) {
            if (blend->is_string()) {
                state.blend_linear = blend->get<std::string>() != "srgb";
            } else if (blend->is_boolean()) {
                state.blend_linear = blend->get<bool>();
            }
        }
        const auto thumbnail = root.find("thumbnail_size");
        if (thumbnail != root.end() && thumbnail->is_number()) {
            // Out of range is clamped, never rejected: a hand-edited 9 is the
            // largest, a -3 the smallest, and neither can break a grid.
            state.thumbnail_size = thumbnail_size_from_number(thumbnail->get<double>());
        }
        const auto output = root.find("output_folder");
        if (output != root.end() && output->is_string()) {
            // A value the folder browser could not have produced (a local path,
            // "..", an overlong or control-character string) is the console.
            const std::string value = output->get<std::string>();
            if (valid_output_folder(value)) state.output_folder = value;
        }
    } catch (const std::exception&) {
        return UiState{};  // garbage: defaults, never a failure
    }
    return state;
}

Status save_ui_state(const Path& state_dir, const UiState& state) {
    std::error_code error;
    std::filesystem::create_directories(state_dir, error);
    if (error) {
        return Status::error("cannot create " + state_dir.string() + ": " + error.message());
    }

    nlohmann::json root;
    root["format_version"] = kStateFormatVersion;
    root["selected_model"] = state.selected_model;
    root["last_album"] = state.last_album;
    root["tooltips"] = state.tooltips;
    root["scale_around"] = state.scale_around == 0
                               ? "front"
                               : (state.scale_around == 1 ? "centre" : "capture");
    root["move_speed"] = state.move_speed;
    root["turn_speed"] = state.turn_speed;
    root["edge_softness"] = state.edge_softness;
    root["blending"] = state.blend_linear ? "linear" : "srgb";
    root["thumbnail_size"] = clamp_thumbnail_size(state.thumbnail_size);
    // Only a value load_ui_state() would accept is written back.
    root["output_folder"] = valid_output_folder(state.output_folder) ? state.output_folder
                                                                     : std::string();
    const std::string text = root.dump(2) + "\n";

    const Path path = ui_state_path(state_dir);
    const Path temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return Status::error("cannot open " + temporary.string() + " for writing");
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            std::filesystem::remove(temporary, error);
            return Status::error("cannot write " + temporary.string());
        }
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return Status::error("cannot replace " + path.string() + ": " + error.message());
    }
    return Status::success();
}

}  // namespace splat
