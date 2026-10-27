// Host-testable part of the PS5 HTTP fetcher: the 200-vs-206 contract and the
// response-header parsing it depends on. Nothing here touches sceNet/sceHttp,
// so tests/http_fetch_contract_test.cpp can verify it on the Mac.
//
// The contract is the one IHttpFetcher documents (core/model/http_fetch.h) and
// the desktop fetcher implements (platform/desktop/curl_http_fetcher.cpp):
// a request for `Range: bytes=<offset>-` answered with
//
//   206 + Content-Range starting at <offset>  -> append to the partial file
//   206 + Content-Range starting elsewhere    -> fail (the file would corrupt)
//   200 (the server ignored Range)            -> truncate and write from 0
//   anything else                             -> fail
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace splat {
namespace ps5 {

struct ResponseHeaders {
    long status = 0;
    bool accept_ranges = false;
    bool has_content_range = false;
    uint64_t content_range_start = 0;
    uint64_t content_range_total = 0;  // 0 for "bytes a-b/*" or absent
    bool has_content_length = false;
    uint64_t content_length = 0;
};

enum class RangeDecision {
    append,   // 206 for the requested offset: keep the bytes already in the file
    restart,  // 200: the server ignored Range, so start the file over
    reject,   // wrong offset, or a status a download cannot use
};

inline std::string lower_ascii(std::string text) {
    for (char& character : text) {
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    }
    return text;
}

// Parses an HTTP response header block ("HTTP/1.1 206 ...\r\nName: value\r\n").
// A redirect chain may contribute several status lines; like the curl fetcher,
// the last one wins and its headers replaced the earlier ones.
inline ResponseHeaders parse_response_headers(const char* data, size_t size) {
    ResponseHeaders headers;
    size_t position = 0;
    while (data != nullptr && position < size) {
        size_t end = position;
        while (end < size && data[end] != '\n') ++end;
        size_t length = end - position;
        if (length > 0 && data[position + length - 1] == '\r') --length;
        const std::string line(data + position, length);
        position = end < size ? end + 1 : size;
        if (line.empty()) continue;

        if (line.rfind("HTTP/", 0) == 0) {
            // A new response starts here; forget the previous hop's headers.
            headers = ResponseHeaders{};
            const size_t space = line.find(' ');
            if (space != std::string::npos) {
                headers.status = std::strtol(line.c_str() + space + 1, nullptr, 10);
            }
            continue;
        }

        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string name = lower_ascii(line.substr(0, colon));
        std::string value = line.substr(colon + 1);
        const size_t first = value.find_first_not_of(" \t");
        value.erase(0, first == std::string::npos ? value.size() : first);
        const size_t last = value.find_last_not_of(" \t\r");
        if (last == std::string::npos) {
            value.clear();
        } else {
            value.erase(last + 1);
        }

        if (name == "accept-ranges") {
            if (lower_ascii(value).find("bytes") != std::string::npos) {
                headers.accept_ranges = true;
            }
        } else if (name == "content-length" && !value.empty()) {
            headers.has_content_length = true;
            headers.content_length = std::strtoull(value.c_str(), nullptr, 10);
        } else if (name == "content-range" && value.rfind("bytes ", 0) == 0) {
            // "bytes 1234-5678/9012"; the total may be "*".
            const size_t dash = value.find('-', 6);
            if (dash == std::string::npos) continue;
            headers.has_content_range = true;
            headers.content_range_start = std::strtoull(value.c_str() + 6, nullptr, 10);
            const size_t slash = value.find('/', dash);
            if (slash != std::string::npos && slash + 1 < value.size() && value[slash + 1] != '*') {
                headers.content_range_total = std::strtoull(value.c_str() + slash + 1, nullptr, 10);
            }
        }
    }
    return headers;
}

// Decides what to do with the body of a GET issued for `offset`.
inline RangeDecision decide_range(uint64_t offset, const ResponseHeaders& headers) {
    if (headers.status != 200 && headers.status != 206) return RangeDecision::reject;
    if (headers.status == 200) return RangeDecision::restart;
    if (offset > 0 && headers.has_content_range && headers.content_range_start != offset) {
        return RangeDecision::reject;
    }
    return RangeDecision::append;
}

// "bytes=<offset>-" for a resume, "bytes=<offset>-<offset+max_bytes-1>" for a
// bounded probe (max_bytes == 0 means "to the end").
inline std::string range_header(uint64_t offset, uint64_t max_bytes) {
    std::string value = "bytes=" + std::to_string(offset) + "-";
    if (max_bytes != 0) value += std::to_string(offset + max_bytes - 1);
    return value;
}

}  // namespace ps5
}  // namespace splat
