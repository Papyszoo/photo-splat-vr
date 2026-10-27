#pragma once

#include <stdexcept>
#include <string>

namespace splat {

// Argument validation shared by every graph builder in core/recon/ggml and
// core/recon/sharp. A graph that does not match the checkpoint throws
// std::runtime_error with the caller's message; the public run() methods catch
// it and return a Status, so no caller outside these files sees an exception.
inline void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

}  // namespace splat
