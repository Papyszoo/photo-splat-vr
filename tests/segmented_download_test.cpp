// The parallel download engine (core/model/segmented_download.*) without the
// model manager: the segment plan, the segments file, and the threads, over an
// in-memory fetcher that honours FetchRequest::end like the real ones.
// tests/model_manager_test.cpp runs the same engine through ModelManager::fetch
// against a real local HTTP server.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "model/segmented_download.h"
#include "test_util.h"

namespace {

using splat::DownloadSegment;
using splat::FetchRequest;
using splat::HttpResource;
using splat::IHttpFetcher;
using splat::Path;
using splat::Result;
using splat::SegmentsState;
using splat::Status;

std::string make_payload(size_t size) {
    std::string payload(size, '\0');
    uint32_t state = 0x2468ace1u;
    for (size_t index = 0; index < size; ++index) {
        state = state * 1664525u + 1013904223u;
        payload[index] = static_cast<char>(state >> 24);
    }
    return payload;
}

void write_file(const Path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string read_file(const Path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// In-memory fetcher for segment requests. fail_at_offset makes the request that
// starts there fail after fail_after bytes; the others go on slowly, one
// 1 KiB chunk per millisecond, until they are told to stop.
class MemorySegmentFetcher final : public IHttpFetcher {
public:
    std::string payload;
    uint64_t fail_at_offset = UINT64_MAX;
    uint64_t fail_after = 0;
    bool end_early = false;  // returns success after half of its range
    std::atomic<int> cancelled_requests{0};
    std::atomic<int> in_flight{0};

    Result<HttpResource> head(const std::string&) override {
        HttpResource resource;
        resource.content_length = payload.size();
        resource.range_supported = true;
        return Result<HttpResource>::success(resource);
    }

    Status download(const FetchRequest& request) override {
        ++in_flight;
        struct Leave {
            std::atomic<int>& counter;
            ~Leave() { --counter; }
        } leave{in_flight};

        std::FILE* file = std::fopen(request.destination.string().c_str(), "r+b");
        if (file == nullptr) return Status::error("cannot open the part");
        std::fseek(file, static_cast<long>(request.offset), SEEK_SET);
        std::setvbuf(file, nullptr, _IONBF, 0);

        uint64_t end = request.end;
        if (end_early) end = request.offset + (request.end - request.offset) / 2;
        uint64_t written = 0;
        Status result = Status::success();
        for (uint64_t at = request.offset; at < end;) {
            if (request.cancelled && request.cancelled()) {
                ++cancelled_requests;
                result = Status::error("download cancelled");
                break;
            }
            if (request.offset == fail_at_offset && written >= fail_after) {
                result = Status::error("connection reset");
                break;
            }
            const size_t take = static_cast<size_t>(std::min<uint64_t>(1024, end - at));
            if (std::fwrite(payload.data() + at, 1, take, file) != take) {
                result = Status::error("write failed");
                break;
            }
            at += take;
            written += take;
            if (request.written != nullptr) request.written->store(written);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::fclose(file);
        return result;
    }

    Result<std::string> get_text(const std::string&, size_t) override {
        return Result<std::string>::failure("not used");
    }
};

std::string segments_json(uint64_t total, const std::string& list, int version = 1) {
    return "{\"version\":" + std::to_string(version) + ",\"total\":" + std::to_string(total) +
           ",\"segments\":" + list + "}";
}

}  // namespace

TEST_CASE("segmented download: plan_segments splits the rest behind a finished prefix") {
    auto segments = splat::plan_segments(0, 10, 4);
    REQUIRE(segments.size() == 4);
    CHECK(segments[0].begin == 0);
    CHECK(segments[0].end == 2);
    CHECK(segments[3].begin == 6);
    CHECK(segments[3].end == 10);  // the last takes the remainder
    for (const DownloadSegment& segment : segments) CHECK(segment.done == 0);

    segments = splat::plan_segments(100, 1000, 4);
    REQUIRE(segments.size() == 5);
    CHECK(segments[0].begin == 0);
    CHECK(segments[0].end == 100);
    CHECK(segments[0].done == 100);  // the prefix is already in the file
    CHECK(segments[1].begin == 100);
    CHECK(segments[4].end == 1000);
    uint64_t next = 0;
    for (const DownloadSegment& segment : segments) {
        CHECK(segment.begin == next);
        next = segment.end;
    }

    CHECK(splat::plan_segments(0, 3, 4).size() == 3);  // never an empty segment
    CHECK(splat::plan_segments(1000, 1000, 4).size() == 1);
    CHECK(splat::plan_segments(1000, 1000, 4)[0].done == 1000);
    CHECK(splat::segments_done(splat::plan_segments(100, 1000, 4)) == 100);
}

TEST_CASE("segmented download: the segments file is checked before it is trusted") {
    splat_test::TempDir temp("segments-file");
    const Path part = temp.path() / "m.bin.part";
    std::vector<DownloadSegment> segments;

    CHECK(splat::read_segments(part, 1000, &segments) == SegmentsState::none);

    write_file(part, std::string(600, 'x'));
    write_file(splat::segments_path(part), segments_json(1000, "[[0,500,500],[500,1000,100]]"));
    REQUIRE(splat::read_segments(part, 1000, &segments) == SegmentsState::valid);
    REQUIRE(segments.size() == 2);
    CHECK(segments[0].done == 500);
    CHECK(segments[1].done == 100);
    CHECK(splat::part_bytes(part, 1000) == 600);

    // The .part is shorter than a segment's recorded bytes: that segment alone is reset.
    write_file(part, std::string(550, 'x'));
    REQUIRE(splat::read_segments(part, 1000, &segments) == SegmentsState::valid);
    CHECK(segments[0].done == 500);
    CHECK(segments[1].done == 0);
    CHECK(splat::part_bytes(part, 1000) == 500);

    write_file(part, std::string(100, 'x'));
    REQUIRE(splat::read_segments(part, 1000, &segments) == SegmentsState::valid);
    CHECK(splat::segments_done(segments) == 0);

    const char* const bad[] = {
        "not json",
        "[]",
        "{\"version\":1}",
        "{\"version\":2,\"total\":1000,\"segments\":[[0,1000,0]]}",   // unknown version
        "{\"version\":1,\"total\":999,\"segments\":[[0,999,0]]}",      // another file size
        "{\"version\":1,\"total\":1000,\"segments\":[]}",
        "{\"version\":1,\"total\":1000,\"segments\":[[0,500,0],[600,1000,0]]}",  // a gap
        "{\"version\":1,\"total\":1000,\"segments\":[[0,600,0],[500,1000,0]]}",  // an overlap
        "{\"version\":1,\"total\":1000,\"segments\":[[0,500,501],[500,1000,0]]}",  // done past the end
        "{\"version\":1,\"total\":1000,\"segments\":[[0,500,0]]}",     // does not reach the end
        "{\"version\":1,\"total\":1000,\"segments\":[[0,500]]}",
    };
    for (const char* text : bad) {
        write_file(splat::segments_path(part), text);
        CHECK_MESSAGE(splat::read_segments(part, 1000, &segments) == SegmentsState::invalid, text);
        CHECK(splat::part_bytes(part, 1000) == 0);
    }

    splat::remove_part_files(part);
    CHECK_FALSE(std::filesystem::exists(part));
    CHECK_FALSE(std::filesystem::exists(splat::segments_path(part)));

    // A plain sequential .part: no segments file, its size is the position.
    write_file(part, std::string(321, 'x'));
    CHECK(splat::read_segments(part, 1000, &segments) == SegmentsState::none);
    CHECK(splat::part_bytes(part, 1000) == 321);
}

TEST_CASE("segmented download: all segments run at once and assemble the exact bytes") {
    splat_test::TempDir temp("segments-run");
    MemorySegmentFetcher fetcher;
    fetcher.payload = make_payload(160000);
    const Path part = temp.path() / "m.bin.part";

    std::atomic<int> peak{0};
    std::atomic<bool> watching{true};
    std::thread watcher([&] {
        while (watching.load()) {
            int seen = peak.load();
            const int now = fetcher.in_flight.load();
            while (now > seen && !peak.compare_exchange_weak(seen, now)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    splat::SegmentedDownload job;
    job.fetcher = &fetcher;
    job.url = "memory://file";
    job.part = part;
    job.total = fetcher.payload.size();
    job.segments = splat::plan_segments(0, job.total, 4);
    splat::Progress progress;
    job.progress = &progress;
    const Status status = splat::download_segmented(job);
    watching.store(false);
    watcher.join();

    REQUIRE_MESSAGE(status.ok(), status.message());
    CHECK(peak.load() == 4);
    CHECK(read_file(part) == fetcher.payload);
    CHECK_FALSE(std::filesystem::exists(splat::segments_path(part)));
    CHECK(progress.fraction() == doctest::Approx(1.0f));
}

TEST_CASE("segmented download: a failing segment stops the others and keeps the exact resume state") {
    splat_test::TempDir temp("segments-fail");
    MemorySegmentFetcher fetcher;
    fetcher.payload = make_payload(400000);
    fetcher.fail_at_offset = 100000;  // the second of four segments
    fetcher.fail_after = 3072;  // three whole chunks
    const Path part = temp.path() / "m.bin.part";

    splat::SegmentedDownload job;
    job.fetcher = &fetcher;
    job.url = "memory://file";
    job.part = part;
    job.total = fetcher.payload.size();
    job.segments = splat::plan_segments(0, job.total, 4);

    const auto started = std::chrono::steady_clock::now();
    const Status failed = splat::download_segmented(job);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    REQUIRE_FALSE(failed.ok());
    CHECK(failed.message() == "connection reset");  // the cause, not a sibling's "cancelled"
    CHECK(fetcher.cancelled_requests.load() == 3);
    CHECK(fetcher.in_flight.load() == 0);
    // The siblings would need 100 000 x 1 ms each; they stopped within moments.
    CHECK(elapsed < std::chrono::seconds(5));

    std::vector<DownloadSegment> segments;
    REQUIRE(splat::read_segments(part, job.total, &segments) == SegmentsState::valid);
    CHECK(segments[1].done == 3072);
    const std::string on_disk = read_file(part);
    for (const DownloadSegment& segment : segments) {
        if (segment.done == 0) continue;  // a segment that had not started has nothing to check
        REQUIRE(on_disk.size() >= segment.begin + segment.done);
        CHECK(on_disk.compare(segment.begin, segment.done, fetcher.payload, segment.begin,
                              segment.done) == 0);
    }

    // The resume finishes the file from those positions.
    fetcher.fail_at_offset = UINT64_MAX;
    job.segments = segments;
    const Status resumed = splat::download_segmented(job);
    REQUIRE_MESSAGE(resumed.ok(), resumed.message());
    CHECK(read_file(part) == fetcher.payload);
}

TEST_CASE("segmented download: a cancel from the caller stops every segment") {
    splat_test::TempDir temp("segments-cancel");
    MemorySegmentFetcher fetcher;
    fetcher.payload = make_payload(400000);
    const Path part = temp.path() / "m.bin.part";

    splat::SegmentedDownload job;
    job.fetcher = &fetcher;
    job.url = "memory://file";
    job.part = part;
    job.total = fetcher.payload.size();
    job.segments = splat::plan_segments(0, job.total, 4);
    const auto started = std::chrono::steady_clock::now();
    job.cancelled = [&started] {
        return std::chrono::steady_clock::now() - started > std::chrono::milliseconds(50);
    };

    const Status stopped = splat::download_segmented(job);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    REQUIRE_FALSE(stopped.ok());
    CHECK(stopped.message() == "download cancelled");
    CHECK(fetcher.cancelled_requests.load() == 4);
    CHECK(fetcher.in_flight.load() == 0);
    CHECK(elapsed < std::chrono::seconds(2));
    CHECK(std::filesystem::exists(splat::segments_path(part)));  // kept for the resume
    CHECK(splat::part_bytes(part, job.total) > 0);
}

TEST_CASE("segmented download: a segment that ends early is a failure, never a finished file") {
    splat_test::TempDir temp("segments-short");
    MemorySegmentFetcher fetcher;
    fetcher.payload = make_payload(8000);
    fetcher.end_early = true;
    const Path part = temp.path() / "m.bin.part";

    splat::SegmentedDownload job;
    job.fetcher = &fetcher;
    job.url = "memory://file";
    job.part = part;
    job.total = fetcher.payload.size();
    job.segments = splat::plan_segments(0, job.total, 4);

    const Status status = splat::download_segmented(job);
    REQUIRE_FALSE(status.ok());
    CHECK(status.message().find("ended after") != std::string::npos);
    CHECK(std::filesystem::exists(splat::segments_path(part)));
}
