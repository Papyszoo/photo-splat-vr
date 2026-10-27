#include "scene/scene.h"

namespace splat {

#ifndef SPLAT_VERSION
#define SPLAT_VERSION "0.0.0-unknown"
#endif

const char* core_version() { return SPLAT_VERSION; }

}  // namespace splat
