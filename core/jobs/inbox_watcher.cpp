#include "jobs/inbox_watcher.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace splat {
namespace {

std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

}  // namespace

InboxWatcher::InboxWatcher(std::vector<Path> directories) : directories_(std::move(directories)) {}

bool InboxWatcher::is_photo_file(const Path& path) {
    const std::string extension = lowercase(path.extension().string());
    return extension == ".jpg" || extension == ".jpeg" || extension == ".png";
}

std::vector<Path> InboxWatcher::poll() {
    std::map<std::string, uintmax_t> seen;
    std::vector<Path> ready;
    std::set<std::string> ready_keys;

    for (const Path& directory : directories_) {
        std::error_code error;
        if (!std::filesystem::is_directory(directory, error)) continue;

        std::filesystem::recursive_directory_iterator it(
            directory, std::filesystem::directory_options::skip_permission_denied, error);
        const std::filesystem::recursive_directory_iterator end;
        while (!error && it != end) {
            const Path path = it->path();
            std::error_code file_error;
            const bool regular = it->is_regular_file(file_error);
            if (file_error) file_error.clear();
            if (!regular || !is_photo_file(path)) {
                it.increment(error);
                continue;
            }

            const uintmax_t size = std::filesystem::file_size(path, file_error);
            if (file_error) {
                it.increment(error);
                continue;
            }

            const std::string key = path.string();
            if (emitted_.count(key) == 0) {
                const auto pending = pending_.find(key);
                if (pending != pending_.end() && pending->second == size) {
                    if (ready_keys.insert(key).second) {
                        ready.push_back(path);
                        emitted_.insert(key);
                    }
                } else {
                    seen[key] = size;
                }
            }
            // Already handed out paths are skipped: no double enqueue.
            it.increment(error);
        }
    }

    pending_ = std::move(seen);

    // Drop the "already handed out" mark once the file is gone, so a photo
    // re-copied into the inbox is picked up again.
    for (auto it = emitted_.begin(); it != emitted_.end();) {
        std::error_code error;
        if (!std::filesystem::exists(*it, error)) {
            it = emitted_.erase(it);
        } else {
            ++it;
        }
    }

    std::sort(ready.begin(), ready.end());
    return ready;
}

}  // namespace splat
