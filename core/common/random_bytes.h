// The one CSPRNG in the tree. Two callers need unpredictable bytes: the ingest
// PIN gate's session tokens (core/ingest/pin_gate.cpp) and the visible-folders
// passcode salt (core/gallery/visibility.cpp). Both must never fall back to
// rand() or a hash of the clock, so the platform detail lives here once.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>

namespace splat {

// Fills `out` with `size` bytes from the system CSPRNG: arc4random on macOS and
// the BSDs (and so the PS5 payload build), getrandom on Linux, /dev/urandom as
// the fallback. A source that cannot answer aborts the process on purpose: a
// salt or a token from a weak source would silently undo the feature that
// depends on it.
void secure_random_bytes(void* out, size_t size);

// `bytes` random bytes as lowercase hex (2 characters per byte).
std::string random_hex(size_t bytes);

}  // namespace splat
