// Desktop implementation of IHttpFetcher on the system libcurl.
//
// The interface only needs HEAD, a ranged GET streamed to a file, and a bounded
// text GET. Everything curl-specific lives in this file so the PS5 backend can
// replace it without touching core/model/model_manager.cpp.

#include "platform/desktop/curl_http_fetcher.h"

#include <curl/curl.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>

namespace splat {
namespace {

void ensure_curl_global() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::string lowercase(std::string text) {
    for (char& character : text) {
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    }
    return text;
}

struct HeaderState {
    long status = 0;
    bool accept_ranges = false;
    bool has_content_range = false;
    uint64_t content_range_start = 0;
};

struct TransferState {
    std::FILE* file = nullptr;
    std::string path;
    uint64_t offset = 0;   // bytes already in the file before this transfer
    uint64_t written = 0;  // bytes written by this transfer
    uint64_t total = 0;
    int last_percent = -1;
    bool first_write = true;
    bool truncate_on_first_write = false;
    bool offsets_mismatch = false;
    bool write_failed = false;
    HeaderState header;
    Progress* progress = nullptr;
    const std::function<bool()>* cancelled = nullptr;
};

struct TextState {
    std::string text;
    size_t max_bytes = 0;
    bool overflow = false;
};

size_t header_callback(char* data, size_t size, size_t count, void* userdata) {
    auto* header = static_cast<HeaderState*>(userdata);
    const size_t bytes = size * count;
    const std::string line(data, bytes);
    if (line.rfind("HTTP/", 0) == 0) {
        // A redirect chain produces several status lines; keep the last one.
        const size_t space = line.find(' ');
        header->status = space == std::string::npos ? 0 : std::strtol(line.c_str() + space + 1, nullptr, 10);
        header->accept_ranges = false;
        header->has_content_range = false;
        header->content_range_start = 0;
        return bytes;
    }
    const size_t colon = line.find(':');
    if (colon == std::string::npos) return bytes;
    const std::string name = lowercase(line.substr(0, colon));
    std::string value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
    if (name == "accept-ranges" && lowercase(value).find("bytes") != std::string::npos) {
        header->accept_ranges = true;
    } else if (name == "content-range") {
        // "bytes 1234-5678/9012"
        const size_t dash = value.find('-');
        if (dash != std::string::npos && value.rfind("bytes ", 0) == 0) {
            header->has_content_range = true;
            header->content_range_start = std::strtoull(value.c_str() + 6, nullptr, 10);
        }
    }
    return bytes;
}

size_t write_callback(char* data, size_t size, size_t count, void* userdata) {
    auto* state = static_cast<TransferState*>(userdata);
    const size_t bytes = size * count;

    if (state->first_write) {
        state->first_write = false;
        if (state->offset > 0 && state->header.status == 200) {
            // The server ignored the Range request: start over instead of
            // appending a second copy of the prefix.
            state->truncate_on_first_write = true;
        }
        if (state->offset > 0 && state->header.status == 206 && state->header.has_content_range &&
            state->header.content_range_start != state->offset) {
            state->offsets_mismatch = true;
            return 0;
        }
        if (state->truncate_on_first_write) {
            std::fclose(state->file);
            state->file = std::fopen(state->path.c_str(), "wb");
            if (state->file == nullptr) {
                state->write_failed = true;
                return 0;
            }
            state->offset = 0;
        }
    }

    if (bytes == 0) return 0;
    if (std::fwrite(data, 1, bytes, state->file) != bytes) {
        state->write_failed = true;
        return 0;
    }
    state->written += bytes;

    if (state->progress != nullptr && state->total > 0) {
        const uint64_t done = state->offset + state->written;
        const int percent = static_cast<int>((done * 1000ull) / state->total);
        if (percent != state->last_percent) {
            state->last_percent = percent;
            state->progress->report(static_cast<float>(static_cast<double>(done) /
                                                       static_cast<double>(state->total)),
                                    "downloading");
        }
    }
    return bytes;
}

int transfer_callback(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* state = static_cast<TransferState*>(userdata);
    if (state->cancelled != nullptr && *state->cancelled && (*state->cancelled)()) return 1;
    return 0;
}

size_t text_callback(char* data, size_t size, size_t count, void* userdata) {
    auto* state = static_cast<TextState*>(userdata);
    const size_t bytes = size * count;
    if (state->max_bytes != 0 && state->text.size() + bytes > state->max_bytes) {
        state->overflow = true;
        return 0;  // aborts the transfer
    }
    state->text.append(data, bytes);
    return bytes;
}

std::string error_with_status(const std::string& url, CURLcode code, long status) {
    if (status >= 400) {
        return "HTTP " + std::to_string(status) + " from " + url;
    }
    return url + ": " + curl_easy_strerror(code);
}

struct CurlHandle {
    CURL* handle = nullptr;
    CurlHandle() { handle = curl_easy_init(); }
    ~CurlHandle() {
        if (handle != nullptr) curl_easy_cleanup(handle);
    }
    CurlHandle(const CurlHandle&) = delete;
    CurlHandle& operator=(const CurlHandle&) = delete;
};

void set_common_options(CURL* curl, const std::string& url) {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ps5-splat-vr");
}

}  // namespace

Result<HttpResource> CurlHttpFetcher::head(const std::string& url) {
    ensure_curl_global();
    CurlHandle curl;
    if (curl.handle == nullptr) return Result<HttpResource>::failure("cannot create a curl handle");

    HeaderState header;
    set_common_options(curl.handle, url);
    curl_easy_setopt(curl.handle, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl.handle, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl.handle, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl.handle, CURLOPT_HEADERDATA, &header);

    const CURLcode code = curl_easy_perform(curl.handle);
    if (code != CURLE_OK) {
        return Result<HttpResource>::failure(error_with_status(url, code, header.status));
    }
    if (header.status != 0 && (header.status < 200 || header.status >= 300)) {
        return Result<HttpResource>::failure("HTTP " + std::to_string(header.status) + " from " + url);
    }

    curl_off_t length = -1;
    curl_easy_getinfo(curl.handle, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
    HttpResource resource;
    resource.content_length = length > 0 ? static_cast<uint64_t>(length) : 0;
    resource.range_supported = header.accept_ranges;
    return Result<HttpResource>::success(resource);
}

Status CurlHttpFetcher::download(const FetchRequest& request) {
    ensure_curl_global();
    if (request.url.empty()) return Status::error("download needs a URL");
    if (request.destination.empty()) return Status::error("download needs a destination path");

    std::error_code directory_error;
    std::filesystem::create_directories(request.destination.parent_path(), directory_error);
    if (directory_error) {
        return Status::error("cannot create " + request.destination.parent_path().string() + ": " +
                             directory_error.message());
    }

    CurlHandle curl;
    if (curl.handle == nullptr) return Status::error("cannot create a curl handle");

    TransferState state;
    state.path = request.destination.string();
    state.offset = request.offset;
    state.total = request.total_bytes;
    state.progress = request.progress;
    state.cancelled = &request.cancelled;
    state.file = std::fopen(state.path.c_str(), request.offset > 0 ? "ab" : "wb");
    if (state.file == nullptr) return Status::error("cannot open " + state.path + " for writing");

    struct Closer {
        TransferState& state;
        ~Closer() {
            if (state.file != nullptr) std::fclose(state.file);
        }
    } closer{state};

    struct curl_slist* headers = nullptr;
    if (request.offset > 0) {
        const std::string range = "Range: bytes=" + std::to_string(request.offset) + "-";
        headers = curl_slist_append(headers, range.c_str());
        curl_easy_setopt(curl.handle, CURLOPT_HTTPHEADER, headers);
    }

    set_common_options(curl.handle, request.url);
    curl_easy_setopt(curl.handle, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl.handle, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl.handle, CURLOPT_HEADERDATA, &state.header);
    curl_easy_setopt(curl.handle, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl.handle, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl.handle, CURLOPT_XFERINFOFUNCTION, transfer_callback);
    curl_easy_setopt(curl.handle, CURLOPT_XFERINFODATA, &state);
    curl_easy_setopt(curl.handle, CURLOPT_NOPROGRESS, 0L);

    const CURLcode code = curl_easy_perform(curl.handle);
    if (headers != nullptr) curl_slist_free_all(headers);

    if (state.offsets_mismatch) {
        return Status::error(request.url + ": the server resumed at the wrong byte offset");
    }
    if (state.write_failed) {
        return Status::error("failed while writing " + request.destination.string());
    }
    if (code == CURLE_ABORTED_BY_CALLBACK) {
        if (request.cancelled && request.cancelled()) return Status::error("download cancelled");
        return Status::error(request.url + ": transfer aborted");
    }
    if (code != CURLE_OK) {
        return Status::error(error_with_status(request.url, code, state.header.status));
    }
    if (state.header.status != 0 && state.header.status != 200 && state.header.status != 206) {
        return Status::error("HTTP " + std::to_string(state.header.status) + " from " + request.url);
    }

    if (std::fflush(state.file) != 0) {
        return Status::error("cannot flush " + request.destination.string());
    }
    return Status::success();
}

Result<std::string> CurlHttpFetcher::get_text(const std::string& url, size_t max_bytes) {
    ensure_curl_global();
    CurlHandle curl;
    if (curl.handle == nullptr) return Result<std::string>::failure("cannot create a curl handle");

    HeaderState header;
    TextState text;
    text.max_bytes = max_bytes;
    set_common_options(curl.handle, url);
    curl_easy_setopt(curl.handle, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl.handle, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl.handle, CURLOPT_HEADERDATA, &header);
    curl_easy_setopt(curl.handle, CURLOPT_WRITEFUNCTION, text_callback);
    curl_easy_setopt(curl.handle, CURLOPT_WRITEDATA, &text);

    const CURLcode code = curl_easy_perform(curl.handle);
    if (text.overflow) {
        return Result<std::string>::failure(url + ": response is larger than " +
                                            std::to_string(max_bytes) + " bytes");
    }
    if (code != CURLE_OK) {
        return Result<std::string>::failure(error_with_status(url, code, header.status));
    }
    return Result<std::string>::success(std::move(text.text));
}

}  // namespace splat
