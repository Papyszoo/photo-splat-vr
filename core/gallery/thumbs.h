#pragma once

#include <cstdint>
#include <vector>

#include "common/path.h"
#include "common/status.h"
#include "ui/image.h"

namespace splat {

// The UI canvas image type (core/ui/image.h): RGBA8, row-major, top row first.
using RgbaImage = UiImage;

// Decodes a photo (JPEG/PNG) and scales it down to fit max_edge, keeping the
// aspect ratio and never upscaling. Alpha is opaque.
Result<RgbaImage> load_photo_thumb(const Path& path, int max_edge = 256);

// Reads <folder>/thumb.jpg. A scene without a thumbnail is not an error the
// caller should surface: it returns a failure and the UI draws its placeholder.
Result<RgbaImage> load_scene_thumb(const Path& folder, int max_edge = 256);

}  // namespace splat
