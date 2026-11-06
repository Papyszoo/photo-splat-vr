// SPDX-License-Identifier: GPL-3.0-or-later

#include "scene/scene_rotate.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/image.h"
#include "common/quat.h"
#include "image/orientation.h"
#include "jobs/image_io.h"
#include "scene/psplat.h"
#include "vfs/io.h"

namespace splat {
namespace {

// qz(quarters * 90 deg) * q, with q in .psplat's (w, x, y, z) order. The world
// rotation composes on the left, R(qz*q) = R(qz) R(q): the renderer builds the
// Gaussian's covariance from R(q) (core/render/project_math.h), so a turn about
// the viewing axis must premultiply. The sign is canonicalized (the first
// non-zero component positive) so four file turns come back to the same
// components, not the negated representative of the same rotation.
void rotate_orientation(float q[4], int quarters) {
    const int turns = ((quarters % 4) + 4) % 4;
    if (turns == 0) return;
    const double angle = turns * (3.14159265358979323846 / 2.0);
    const float c = static_cast<float>(std::cos(angle / 2.0));
    const float s = static_cast<float>(std::sin(angle / 2.0));
    const float qz[4] = {c, 0.0f, 0.0f, s};
    float turned[4];
    quat_mul(turned, qz, q);
    // Unit length up to rounding; the renderer normalises but the file should
    // not drift.
    quat_unit(q, turned);
    for (int i = 0; i < 4; ++i) {
        if (std::fabs(q[i]) <= 1.0e-7f) continue;
        if (q[i] < 0.0f) {
            for (int j = 0; j < 4; ++j) q[j] = -q[j];
        }
        break;
    }
}

// Turns one JPEG file and writes the replacement beside it. False when the file
// is absent (an imported scene has no original.jpg) or cannot be decoded; a
// missing thumb.jpg is not fatal either.
Status turn_jpeg(const Path& source, const Path& temporary, int quarters, int quality) {
    vfs::FileStat inspected;
    const Status stat = vfs::stat(source, &inspected);
    if (!stat.ok()) return stat;
    if (!inspected.exists) return Status::success();
    if (!inspected.regular) return Status::error(source.string() + " is not a file");

    const Result<std::vector<uint8_t>> bytes = vfs::read_file(source);
    if (!bytes.ok()) return Status::error(bytes.message());
    const Result<Image> decoded = decode_image(bytes.value().data(), bytes.value().size());
    if (!decoded.ok()) return Status::error("cannot decode " + source.string() + ": " +
                                           decoded.message());
    Image image = decoded.value();
    image::rotate_quarters(image, quarters);
    const Result<std::vector<uint8_t>> encoded = encode_jpeg(image, quality);
    if (!encoded.ok()) return Status::error(encoded.message());
    return vfs::write_file(temporary, encoded.value());
}

// The intrinsics object in meta.json. Missing or non-numeric fields are left
// alone; a file with an absent "intrinsics" object keeps that shape.
void turn_meta_intrinsics(nlohmann::json& root, int quarters) {
    const auto intrinsics = root.find("intrinsics");
    if (intrinsics == root.end() || !intrinsics->is_object()) return;
    const auto number = [&intrinsics](const char* key, double* out) {
        const auto value = intrinsics->find(key);
        if (value == intrinsics->end() || !value->is_number()) return false;
        *out = value->get<double>();
        return true;
    };
    double width = 0.0, height = 0.0, fx = 0.0, fy = 0.0, cx = 0.0, cy = 0.0;
    if (!number("width", &width) || !number("height", &height)) return;
    const bool has_focal = number("fx", &fx);
    const bool has_fy = number("fy", &fy);
    const bool has_cx = number("cx", &cx);
    const bool has_cy = number("cy", &cy);

    image::ImageIntrinsics mapped;
    mapped.width = static_cast<int>(width);
    mapped.height = static_cast<int>(height);
    mapped.fx = static_cast<float>(fx);
    mapped.fy = static_cast<float>(fy);
    mapped.cx = static_cast<float>(cx);
    mapped.cy = static_cast<float>(cy);
    const image::ImageIntrinsics turned = image::orient_intrinsics(mapped, 1, quarters);

    (*intrinsics)["width"] = static_cast<uint32_t>(std::max(0, turned.width));
    (*intrinsics)["height"] = static_cast<uint32_t>(std::max(0, turned.height));
    if (has_focal) (*intrinsics)["fx"] = turned.fx;
    if (has_fy) (*intrinsics)["fy"] = turned.fy;
    if (has_cx) (*intrinsics)["cx"] = turned.cx;
    if (has_cy) (*intrinsics)["cy"] = turned.cy;
}

// A legacy saved rotation (Track R) is removed; a scale and any unknown member
// of "view" survive. An empty "view" goes entirely.
void drop_legacy_rotation(nlohmann::json& root) {
    const auto view = root.find("view");
    if (view == root.end()) return;
    if (!view->is_object()) {
        root.erase("view");
        return;
    }
    view->erase("rotation");
    if (view->empty()) root.erase("view");
}

}  // namespace

void rotate_scene_quarters(SplatScene& scene, int quarters) {
    const int turns = ((quarters % 4) + 4) % 4;
    if (turns == 0) return;
    // Positions: the image-plane map for a clockwise turn, (x, y) -> (-y, x).
    // Exact for the multiples of 90 degrees, so four turns restore the floats
    // bit for bit.
    for (size_t i = 0; i + 2 < scene.position.size(); i += 3) {
        const float x = scene.position[i];
        const float y = scene.position[i + 1];
        switch (turns) {
            case 1:
                scene.position[i] = -y;
                scene.position[i + 1] = x;
                break;
            case 2:
                scene.position[i] = -x;
                scene.position[i + 1] = -y;
                break;
            case 3:
                scene.position[i] = y;
                scene.position[i + 1] = -x;
                break;
            default:
                break;
        }
    }
    for (size_t i = 0; i + 3 < scene.rotation.size(); i += 4) {
        rotate_orientation(&scene.rotation[i], turns);
    }
    // log_scale, sh_dc and opacity_logit are rotation-invariant: the Gaussian's
    // local frame turns with the world (R S S^T R^T), and degree-0 SH is a
    // constant. .psplat has no higher-order SH terms to re-project. The splat
    // order is not an index: the renderer sorts every frame.
}

Intrinsics rotate_scene_intrinsics(const Intrinsics& intrinsics, int quarters) {
    image::ImageIntrinsics source;
    source.width = static_cast<int>(intrinsics.width);
    source.height = static_cast<int>(intrinsics.height);
    source.fx = intrinsics.fx;
    source.fy = intrinsics.fy;
    source.cx = intrinsics.cx;
    source.cy = intrinsics.cy;
    const image::ImageIntrinsics turned = image::orient_intrinsics(source, 1, quarters);
    Intrinsics out;
    out.width = static_cast<uint32_t>(std::max(0, turned.width));
    out.height = static_cast<uint32_t>(std::max(0, turned.height));
    out.fx = turned.fx;
    out.fy = turned.fy;
    out.cx = turned.cx;
    out.cy = turned.cy;
    return out;
}

Status rotate_scene_folder(const Path& folder, int quarters, int jpeg_quality) {
    const int turns = ((quarters % 4) + 4) % 4;
    if (turns == 0) return Status::success();
    const SceneFiles files = scene_files(folder);

    // ---- read everything before replacing anything ------------------------
    std::string meta_text;
    const Status meta_read = vfs::read_text_file(files.meta, &meta_text);
    if (!meta_read.ok()) return Status::error(meta_read.message());
    nlohmann::json meta;
    try {
        meta = nlohmann::json::parse(meta_text);
    } catch (const std::exception& error) {
        return Status::error(std::string("invalid meta.json in ") + files.meta.string() + ": " +
                             error.what());
    }
    if (!meta.is_object()) {
        return Status::error("meta.json root is not an object: " + files.meta.string());
    }

    PsplatInfo info;
    Result<SplatScene> read = read_psplat(files.splat, &info);
    if (!read.ok()) return Status::error("cannot read scene.psplat: " + read.message());
    SplatScene scene = read.take();
    if (!scene.consistent()) return Status::error("scene.psplat arrays are inconsistent");
    rotate_scene_quarters(scene, turns);

    // The header carries the focal length and capture image size for readers
    // that have no meta.json; meta.json's intrinsics are the fuller answer.
    // Turn whichever the folder actually has.
    float focal = info.focal_px;
    uint32_t image_width = info.image_width;
    uint32_t image_height = info.image_height;
    bool meta_intrinsics = false;
    float meta_fx = 0.0f, meta_fy = 0.0f;
    uint32_t meta_width = 0, meta_height = 0;
    {
        const auto intrinsics = meta.find("intrinsics");
        if (intrinsics != meta.end() && intrinsics->is_object()) {
            const auto width = intrinsics->find("width");
            const auto height = intrinsics->find("height");
            const auto fx = intrinsics->find("fx");
            if (width != intrinsics->end() && height != intrinsics->end() &&
                width->is_number() && height->is_number() && fx != intrinsics->end() &&
                fx->is_number()) {
                meta_width = width->get<uint32_t>();
                meta_height = height->get<uint32_t>();
                meta_fx = fx->get<float>();
                const auto fy = intrinsics->find("fy");
                meta_fy = fy != intrinsics->end() && fy->is_number() ? fy->get<float>() : meta_fx;
                meta_intrinsics = meta_fx > 0.0f;
            }
        }
    }
    if (meta_intrinsics) {
        const Intrinsics turned = rotate_scene_intrinsics(
            Intrinsics{meta_width, meta_height, meta_fx, meta_fy, 0.0f, 0.0f}, turns);
        focal = turned.fx;
        image_width = turned.width;
        image_height = turned.height;
    } else if (turns % 2 == 1) {
        std::swap(image_width, image_height);
    }

    // ---- write every replacement beside its target ------------------------
    const Path temp_splat = vfs::temporary_beside(files.splat);
    const Path temp_original = vfs::temporary_beside(files.original);
    const Path temp_thumb = vfs::temporary_beside(files.thumbnail);
    const Path temp_meta = vfs::temporary_beside(files.meta);
    const Path temporaries[] = {temp_splat, temp_original, temp_thumb, temp_meta};
    auto remove_temporaries = [&temporaries] {
        for (const Path& temporary : temporaries) (void)vfs::remove_file(temporary);
    };
    remove_temporaries();

    Status written = write_psplat(temp_splat, scene, focal, image_width, image_height);
    if (!written.ok()) {
        remove_temporaries();
        return written;
    }
    written = turn_jpeg(files.original, temp_original, turns, jpeg_quality);
    if (!written.ok()) {
        remove_temporaries();
        return Status::error("cannot turn original.jpg: " + written.message());
    }
    written = turn_jpeg(files.thumbnail, temp_thumb, turns, jpeg_quality);
    if (!written.ok()) {
        remove_temporaries();
        return Status::error("cannot turn thumb.jpg: " + written.message());
    }
    turn_meta_intrinsics(meta, turns);
    drop_legacy_rotation(meta);
    // Keep the count the scene now has: it is unchanged by a turn, and this
    // also repairs a meta.json that recorded a stale count.
    meta["splat_count"] = static_cast<uint32_t>(scene.count());
    const std::string meta_out = meta.dump(2) + "\n";
    written = vfs::write_file(temp_meta, reinterpret_cast<const uint8_t*>(meta_out.data()),
                              meta_out.size());
    if (!written.ok()) {
        remove_temporaries();
        return written;
    }

    // ---- publish, in the order that keeps the scene openable --------------
    // count(meta) == count(scene.psplat) either way, so every intermediate
    // state loads. meta.json last: it is the point at which the intrinsics
    // describe the turned picture, and a failure before it leaves the old
    // intrinsics with new geometry (reported), never an unreadable scene.
    const Path targets[] = {files.splat, files.original, files.thumbnail, files.meta};
    const Path sources[] = {temp_splat, temp_original, temp_thumb, temp_meta};
    for (int i = 0; i < 4; ++i) {
        if (i == 1 || i == 2) {
            // original.jpg may be absent (an import has none): then there is no
            // temp either, and the target must be left alone.
            vfs::FileStat inspected;
            if (!vfs::stat(sources[i], &inspected).ok() || !inspected.exists) continue;
        }
        const Status renamed = vfs::rename_file(sources[i], targets[i]);
        if (!renamed.ok()) {
            remove_temporaries();
            return Status::error("cannot replace " + targets[i].string() + ": " +
                                 renamed.message());
        }
    }
    remove_temporaries();  // any skipped image's temp (none on the normal path)
    return Status::success();
}

}  // namespace splat
