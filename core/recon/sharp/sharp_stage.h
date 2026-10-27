#pragma once

// The stage-dump type lives in core/recon/ggml: the DA2 (C4) and SHARP (C3)
// drivers report through the same struct, so a parity helper takes either.
// `SharpStage` is kept as the name the C3 files and tests use.

#include "recon/ggml/stage.h"

namespace splat {

using SharpStage = StageDump;
using SharpStageCallback = StageCallback;

}  // namespace splat
