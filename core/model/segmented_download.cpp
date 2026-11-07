// SPDX-License-Identifier: GPL-3.0-or-later

#include "model/segmented_download.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <system_error>
#include <thread>

#include <nlohmann/json.hpp>

namespace splat {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kSegmentsFormat = 1;
constexpr uint64_t kMaxSegmentsBytes = 64 * 1024;
constexpr size_t kMaxSegments = 64;
// How often the calling thread looks at the transfers (progress, cancel), and
// how often it saves the resume state while bytes keep arriving.
constexpr std::chrono::milliseconds kPollInterval{100};
constexpr std::chrono::seconds kSaveInterval{2};

uint64_t size_or_zero(const Path& path) {
    std::error_code error;
    const uintmax_t size = std::filesystem::file_size(path, error);
    return error ? 0 : static_cast<uint64_t>(size);
}

void remove_file(const Path& path) {
    std::error_code error;
    std::filesystem::remove(path, error);
}

Status write_segments(const Path& part, uint64_t total, const std::vector<DownloadSegment>& segments) {
    nlohmann::json root;
    root["version"] = kSegmentsFormat;
    root["total"] = total;
    nlohmann::json list = nlohmann::json::array();
    for (const DownloadSegment& segment : segments) {
        list.push_back({segment.begin, segment.end, segment.done});
    }
    root["segments"] = std::move(list);
    const std::string text = root.dump() + "\n";

    const Path path = segments_path(part);
    const Path temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return Status::error("cannot open " + temporary.string() + " for writing");
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) return Status::error("failed while writing " + temporary.string());
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        remove_file(temporary);
        return Status::error("cannot replace " + path.string() + ": " + error.message());
    }
    return Status::success();
}

}  // namespace

Path segments_path(const Path& part) { return part.string() + ".segments"; }

void remove_part_files(const Path& part) {
    remove_file(part);
    remove_file(segments_path(part));
    remove_file(segments_path(part).string() + ".tmp");
}

std::vector<DownloadSegment> plan_segments(uint64_t prefix, uint64_t total, int count) {
    std::vector<DownloadSegment> segments;
    prefix = std::min(prefix, total);
    if (prefix > 0) segments.push_back({0, prefix, prefix});
    const uint64_t remaining = total - prefix;
    if (remaining == 0) return segments;

    const uint64_t parts = std::max<uint64_t>(1, std::min<uint64_t>(static_cast<uint64_t>(std::max(count, 1)), remaining));
    const uint64_t each = remaining / parts;
    for (uint64_t index = 0; index < parts; ++index) {
        const uint64_t begin = prefix + index * each;
        segments.push_back({begin, index + 1 == parts ? total : begin + each, 0});
    }
    return segments;
}

uint64_t segments_done(const std::vector<DownloadSegment>& segments) {
    uint64_t done = 0;
    for (const DownloadSegment& segment : segments) done += segment.done;
    return done;
}

SegmentsState read_segments(const Path& part, uint64_t total, std::vector<DownloadSegment>* segments) {
    segments->clear();
    const Path path = segments_path(part);
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error) return SegmentsState::none;

    std::ifstream in(path, std::ios::binary);
    if (!in) return SegmentsState::invalid;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.size() > kMaxSegmentsBytes) return SegmentsState::invalid;

    std::vector<DownloadSegment> parsed;
    try {
        const nlohmann::json root = nlohmann::json::parse(text);
        if (root.at("version").get<uint32_t>() != kSegmentsFormat) return SegmentsState::invalid;
        if (root.at("total").get<uint64_t>() != total) return SegmentsState::invalid;
        const nlohmann::json& list = root.at("segments");
        if (!list.is_array() || list.empty() || list.size() > kMaxSegments) return SegmentsState::invalid;
        for (const nlohmann::json& entry : list) {
            if (!entry.is_array() || entry.size() != 3) return SegmentsState::invalid;
            parsed.push_back({entry[0].get<uint64_t>(), entry[1].get<uint64_t>(), entry[2].get<uint64_t>()});
        }
    } catch (const std::exception&) {
        return SegmentsState::invalid;
    }

    // The segments must tile [0, total) in order.
    uint64_t next = 0;
    for (const DownloadSegment& segment : parsed) {
        if (segment.begin != next || segment.end <= segment.begin ||
            segment.done > segment.end - segment.begin) {
            return SegmentsState::invalid;
        }
        next = segment.end;
    }
    if (next != total) return SegmentsState::invalid;

    // A segment whose recorded bytes are not in the .part (the file was cut or
    // replaced) is fetched again from its start; the others keep their progress.
    const uint64_t held = size_or_zero(part);
    for (DownloadSegment& segment : parsed) {
        if (segment.done > 0 && segment.begin + segment.done > held) segment.done = 0;
    }
    *segments = std::move(parsed);
    return SegmentsState::valid;
}

uint64_t part_bytes(const Path& part, uint64_t total) {
    std::vector<DownloadSegment> segments;
    switch (read_segments(part, total, &segments)) {
        case SegmentsState::valid:
            return segments_done(segments);
        case SegmentsState::invalid:
            return 0;
        case SegmentsState::none:
            break;
    }
    return size_or_zero(part);
}

Status download_segmented(const SegmentedDownload& job) {
    if (job.fetcher == nullptr) return Status::error("a segmented download needs a fetcher");
    if (job.segments.empty() || job.total == 0) {
        return Status::error("a segmented download needs a segment layout");
    }
    const std::vector<DownloadSegment>& segments = job.segments;
    const size_t count = segments.size();

    std::error_code error;
    const Path parent = job.part.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) return Status::error("cannot create " + parent.string() + ": " + error.message());
    }

    // The layout is saved before the .part gets any data: a .part with data but
    // no layout would be read as a sequential prefix on the next resume.
    Status saved = write_segments(job.part, job.total, segments);
    if (!saved.ok()) return saved;
    if (!std::filesystem::exists(job.part, error)) {
        std::ofstream create(job.part, std::ios::binary | std::ios::app);
        if (!create) return Status::error("cannot create " + job.part.string());
    } else if (size_or_zero(job.part) > job.total) {
        std::filesystem::resize_file(job.part, job.total, error);
        if (error) return Status::error("cannot resize " + job.part.string() + ": " + error.message());
    }

    // Per-segment bytes written by this run; the segment's progress is
    // `done + written`.
    std::vector<std::atomic<uint64_t>> written(count);
    for (std::atomic<uint64_t>& counter : written) counter.store(0);
    const auto snapshot = [&]() {
        std::vector<DownloadSegment> now = segments;
        for (size_t index = 0; index < count; ++index) {
            const uint64_t length = now[index].end - now[index].begin;
            now[index].done = std::min(length, now[index].done + written[index].load());
        }
        return now;
    };

    std::atomic<bool> stop{false};
    std::mutex mutex;
    std::condition_variable wake;
    size_t running = 0;  // guarded by mutex
    Status first_error;  // guarded by mutex
    for (const DownloadSegment& segment : segments) {
        if (segment.done < segment.end - segment.begin) ++running;
    }

    std::vector<std::thread> threads;
    for (size_t index = 0; index < count; ++index) {
        if (segments[index].done >= segments[index].end - segments[index].begin) continue;
        threads.emplace_back([&, index] {
            const DownloadSegment& segment = segments[index];
            FetchRequest request;
            request.url = job.url;
            request.destination = job.part;
            request.offset = segment.begin + segment.done;
            request.end = segment.end;
            request.total_bytes = segment.end - segment.begin;
            request.written = &written[index];
            request.cancelled = [&stop] { return stop.load(std::memory_order_relaxed); };

            Status status = job.fetcher->download(request);
            const uint64_t wanted = segment.end - request.offset;
            if (status.ok() && written[index].load() != wanted) {
                status = Status::error(job.url + ": a segment ended after " +
                                       std::to_string(written[index].load()) + " of " +
                                       std::to_string(wanted) + " bytes");
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!status.ok()) {
                    if (first_error.ok()) first_error = status;
                    stop.store(true);  // after the cause is recorded: siblings only echo "cancelled"
                }
                --running;
            }
            wake.notify_all();
        });
    }

    uint64_t saved_done = segments_done(segments);
    Clock::time_point last_saved = Clock::now();
    for (;;) {
        if (!stop.load() && job.cancelled && job.cancelled()) stop.store(true);

        const std::vector<DownloadSegment> now = snapshot();
        const uint64_t done = segments_done(now);
        if (job.progress != nullptr) {
            job.progress->report(
                static_cast<float>(static_cast<double>(done) / static_cast<double>(job.total)),
                "downloading");
        }
        if (done != saved_done && Clock::now() - last_saved >= kSaveInterval) {
            write_segments(job.part, job.total, now);  // best effort: the final save reports
            saved_done = done;
            last_saved = Clock::now();
        }

        std::unique_lock<std::mutex> lock(mutex);
        if (running == 0) break;
        wake.wait_for(lock, kPollInterval, [&] { return running == 0; });
    }
    for (std::thread& thread : threads) thread.join();

    const std::vector<DownloadSegment> final_segments = snapshot();
    bool complete = true;
    for (const DownloadSegment& segment : final_segments) {
        if (segment.done != segment.end - segment.begin) complete = false;
    }
    if (!complete || !first_error.ok()) {
        write_segments(job.part, job.total, final_segments);  // the resume position
        if (!first_error.ok()) return first_error;
        return Status::error("the download stopped before the end of the file");
    }

    remove_file(segments_path(job.part));
    if (job.progress != nullptr) job.progress->report(1.0f, "downloading");
    return Status::success();
}

}  // namespace splat
