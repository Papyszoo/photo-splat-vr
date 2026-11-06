#include "jobs/inbox_watcher.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

#include "image/format.h"
#include "vfs/io.h"

namespace splat {
namespace {

// The tree below one watched directory. The inbox is a flat folder in
// practice; the recursion keeps the old behaviour for a folder an operator
// organised by hand.
void collect(const Path& directory, std::vector<std::pair<Path, uint64_t>>* out) {
    std::vector<vfs::DirEntry> entries;
    if (!vfs::list(directory, &entries).ok()) return;
    for (const vfs::DirEntry& entry : entries) {
        if (entry.symlink) continue;
        const Path path = directory / entry.name;
        if (entry.directory) {
            collect(path, out);
        } else if (is_photo_file(path)) {
            out->push_back({path, entry.size});
        }
    }
}

}  // namespace

InboxWatcher::InboxWatcher(std::vector<Path> directories) : directories_(std::move(directories)) {}

bool InboxWatcher::is_photo_file(const Path& path) { return splat::is_photo_file(path); }

std::vector<Path> InboxWatcher::poll() {
    std::map<std::string, uintmax_t> seen;
    std::vector<Path> ready;
    std::set<std::string> ready_keys;

    for (const Path& directory : directories_) {
        vfs::FileStat status;
        if (!vfs::stat(directory, &status).ok() || !status.directory) continue;

        std::vector<std::pair<Path, uint64_t>> files;
        collect(directory, &files);
        for (const std::pair<Path, uint64_t>& file : files) {
            const std::string key = file.first.string();
            if (emitted_.count(key) != 0) continue;  // already handed out: no double enqueue
            const auto pending = pending_.find(key);
            if (pending != pending_.end() && pending->second == file.second) {
                if (ready_keys.insert(key).second) {
                    ready.push_back(file.first);
                    emitted_.insert(key);
                }
            } else {
                seen[key] = file.second;
            }
        }
    }

    pending_ = std::move(seen);

    // Drop the "already handed out" mark once the file is gone, so a photo
    // re-copied into the inbox is picked up again.
    for (auto it = emitted_.begin(); it != emitted_.end();) {
        vfs::FileStat status;
        if (!vfs::stat(Path(*it), &status).ok() || !status.exists) {
            it = emitted_.erase(it);
        } else {
            ++it;
        }
    }

    std::sort(ready.begin(), ready.end());
    return ready;
}

}  // namespace splat
