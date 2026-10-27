#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "common/path.h"

namespace splat {

// C9. Polls the inbox and attached drives for photos. Scans the configured
// directories recursively and accepts .jpg/.jpeg/.png (case-insensitive).
//
// A file is only returned when two consecutive polls saw the same size, so a
// copy in progress is not ingested halfway. Once returned, a path is not
// returned again until it disappears from the inbox.
class InboxWatcher {
public:
    explicit InboxWatcher(std::vector<Path> directories);

    // One poll step. Returns the newly stable photo paths, sorted.
    std::vector<Path> poll();

    static bool is_photo_file(const Path& path);

private:
    std::vector<Path> directories_;
    std::map<std::string, uintmax_t> pending_;  // seen once, size from the last poll
    std::set<std::string> emitted_;
};

}  // namespace splat
