// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/ui_state.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#include <nlohmann/json.hpp>

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

}  // namespace

Path ui_state_path(const Path& state_dir) { return state_dir / "ui.json"; }

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
                state.scale_around = value == 1 ? 1 : (value == 2 ? 2 : 0);
            } else if (scale->is_string()) {
                const std::string value = scale->get<std::string>();
                state.scale_around = value == "centre" ? 1 : (value == "maquette" ? 2 : 0);
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
    root["scale_around"] = state.scale_around == 1
                               ? "centre"
                               : (state.scale_around == 2 ? "maquette" : "front");
    root["move_speed"] = state.move_speed;
    root["turn_speed"] = state.turn_speed;
    root["edge_softness"] = state.edge_softness;
    root["blending"] = state.blend_linear ? "linear" : "srgb";
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
