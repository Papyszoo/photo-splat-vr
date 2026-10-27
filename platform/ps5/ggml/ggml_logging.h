// Route ggml's own log lines (device name, "matrix cores: coopmat1/2/none",
// UMA/fp16/subgroup/shared-memory facts) to the console log.
//
// ggml has no log-level filter: every GGML_LOG_DEBUG line reaches the callback.
// The Vulkan backend logs its device description there, which is how the
// op_audit log says whether RADV exposed cooperative matrices for that run.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <ps5/klog.h>

#include "ggml.h"

namespace splat {
namespace bench {

inline void install_ggml_logging(const char* tag) {
    ggml_log_set(
        [](enum ggml_log_level level, const char* text, void* user_data) {
            const char* tag = static_cast<const char*>(user_data);
            const char* kind = "info";
            if (level == GGML_LOG_LEVEL_ERROR) kind = "error";
            else if (level == GGML_LOG_LEVEL_WARN) kind = "warn";
            else if (level == GGML_LOG_LEVEL_DEBUG) kind = "debug";
            // text already ends in a newline; keep one line per klog entry.
            char line[1024];
            std::snprintf(line, sizeof(line), "%s: ggml %s: %s", tag, kind, text);
            size_t end = std::strlen(line);
            while (end > 0 && (line[end - 1] == '\n' || line[end - 1] == '\r')) line[--end] = '\0';
            std::printf("%s\n", line);
            std::fflush(stdout);
            klog_puts(line);
        },
        const_cast<char*>(tag));
}

}  // namespace bench
}  // namespace splat
