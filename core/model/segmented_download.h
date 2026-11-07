// Parallel ranged download of one large file (0.6.0).
//
// The console's HTTP stack caps one connection at about 3-4 MB/s while four
// connections together reach about 11 MB/s (runs/v0.2-library/REPORT-060-probe.md),
// so a big file with a known size on a server that offers byte ranges is split
// into a few segments, each its own ranged request writing into the one .part
// file at its own position. The fetcher only has to do a bounded in-place
// request (FetchRequest::end); the split, the threads, the progress and the
// resume state are here, so the Mac tests cover them and every fetcher gets the
// speed-up.
//
// Resume state is `<part>.segments`, a small JSON file next to the .part:
//
//   {"version":1,"total":2809738232,"segments":[[begin,end,done],...]}
//
// Segment k owns the bytes [begin, end) and has written [begin, begin + done)
// in order. The file is rewritten (atomically) every couple of seconds, when a
// download stops and when it fails, so an interrupted download resumes every
// segment where it stopped. The SHA-256 check in ModelManager::fetch still
// proves the finished file.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "common/path.h"
#include "common/progress.h"
#include "common/status.h"
#include "model/http_fetch.h"

namespace splat {

// A file smaller than this (or with less than this left to fetch) is one plain
// GET, as before; four connections only pay off for a big transfer.
inline constexpr uint64_t kSegmentedMinBytes = 64ull * 1024ull * 1024ull;
inline constexpr int kSegmentedCount = 4;

struct DownloadSegment {
    uint64_t begin = 0;
    uint64_t end = 0;   // exclusive
    uint64_t done = 0;  // [begin, begin + done) is in the .part
};

// `<part>.segments`.
Path segments_path(const Path& part);

// Deletes the .part and its segments file.
void remove_part_files(const Path& part);

// Splits [prefix, total) into at most `count` equal segments, behind one
// finished segment [0, prefix) when a sequential download already holds that
// prefix. Needs total > 0.
std::vector<DownloadSegment> plan_segments(uint64_t prefix, uint64_t total, int count);

uint64_t segments_done(const std::vector<DownloadSegment>& segments);

enum class SegmentsState {
    none,     // no segments file: a plain sequential .part (its size is the resume offset) or nothing
    valid,    // read; a segment whose bytes the .part no longer holds is reset to done = 0
    invalid,  // unreadable or inconsistent with `total`: nothing in the .part can be trusted
};
SegmentsState read_segments(const Path& part, uint64_t total, std::vector<DownloadSegment>* segments);

// Bytes of an interrupted download that are certainly on disk: the sum over a
// valid segments file, 0 for an invalid one, the size of a sequential .part.
uint64_t part_bytes(const Path& part, uint64_t total);

struct SegmentedDownload {
    IHttpFetcher* fetcher = nullptr;
    std::string url;
    Path part;
    uint64_t total = 0;
    // The layout to run: plan_segments() for a new download, or the segments
    // read_segments() found for a resume. Finished segments are skipped.
    std::vector<DownloadSegment> segments;
    // 0..1 over the whole file, resumed bytes included. Reported from the
    // calling thread only. May be null.
    Progress* progress = nullptr;
    // Polled from the calling thread; true stops every segment. The .part and
    // its segments file stay for a later resume.
    std::function<bool()> cancelled;
};

// Runs every unfinished segment on its own thread and returns when all have
// finished or stopped. Success means every byte of [0, total) is in the .part;
// the segments file is gone then. A failure or a cancel stops all segments and
// leaves .part + segments file for a resume.
Status download_segmented(const SegmentedDownload& job);

}  // namespace splat
