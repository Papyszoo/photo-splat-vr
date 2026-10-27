// W5. Title-side helper client (see helper_client.h). The mailbox is three
// files under the helper directory, so this file needs no socket: it writes
// req.json, then poll() looks for resp.json. start_helper is the only PS5-only
// part and is compiled out on the host.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "capture/helper_client.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#if defined(PS5_TITLE) || defined(__PROSPERO__)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace splat {
namespace {

constexpr int kElfldrPort = 9021;

std::string read_text(const Path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::string();
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

long long unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

HelperClient::HelperClient(Path helper_dir) : HelperClient(std::move(helper_dir), Timing()) {}

HelperClient::HelperClient(Path helper_dir, Timing timing)
    : helper_dir_(std::move(helper_dir)), timing_(timing) {}

bool HelperClient::alive() const {
    std::ifstream file(helper_dir_ / kHelperAliveName);
    long long stamp = 0;
    if (!(file >> stamp)) return false;
    const double age = static_cast<double>(unix_now() - stamp);
    // A clock that jumped backwards must not make a dead helper look alive for
    // ever; both directions use the same window.
    return age <= timing_.alive_limit_seconds && age >= -timing_.alive_limit_seconds;
}

Status HelperClient::send(const std::string& body) {
    std::error_code error;
    std::filesystem::create_directories(helper_dir_, error);
    if (error) return Status::error("cannot create the helper directory: " + error.message());

    const Path temporary = helper_dir_ / "req.tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return Status::error("cannot write the helper request");
        file << body;
        if (!file) return Status::error("cannot write the helper request");
    }
    std::filesystem::rename(temporary, helper_dir_ / kHelperRequestName, error);
    if (error) return Status::error("cannot publish the helper request: " + error.message());
    return Status::success();
}

Status HelperClient::request_list() {
    if (!alive()) {
        state_ = State::NoHelper;
        error_ = "helper not running";
        return Status::error(error_);
    }

    captures_.clear();
    copied_.clear();
    error_.clear();
    pending_id_ = ++next_id_;

    nlohmann::ordered_json request;
    request["id"] = pending_id_;
    request["op"] = "list";
    const Status status = send(request.dump());
    if (!status) {
        pending_id_ = -1;
        state_ = State::Failed;
        error_ = status.message();
        return status;
    }
    deadline_ = std::chrono::steady_clock::now() +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(timing_.timeout_seconds));
    state_ = State::Waiting;
    return Status::success();
}

Status HelperClient::request_copy(std::vector<Path> paths) {
    if (!alive()) {
        state_ = State::NoHelper;
        error_ = "helper not running";
        return Status::error(error_);
    }

    captures_.clear();
    copied_.clear();
    error_.clear();
    pending_id_ = ++next_id_;

    nlohmann::ordered_json request;
    request["id"] = pending_id_;
    request["op"] = "copy";
    request["paths"] = nlohmann::ordered_json::array();
    for (const Path& path : paths) request["paths"].push_back(path.string());
    const Status status = send(request.dump());
    if (!status) {
        pending_id_ = -1;
        state_ = State::Failed;
        error_ = status.message();
        return status;
    }
    deadline_ = std::chrono::steady_clock::now() +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(timing_.timeout_seconds));
    state_ = State::Waiting;
    return Status::success();
}

bool HelperClient::response_matches(const std::string& text) const {
    const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) return false;
    if (!root.contains("id") || !root["id"].is_number_integer()) return false;
    return root["id"].get<long long>() == pending_id_;
}

void HelperClient::handle_response(const std::string& text) {
    const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) return;

    captures_.clear();
    copied_.clear();
    error_ = root.value("error", std::string());

    if (root.contains("captures") && root["captures"].is_array()) {
        for (const nlohmann::json& entry : root["captures"]) {
            if (!entry.is_object()) continue;
            CaptureInfo capture;
            capture.path = Path(entry.value("path", std::string()));
            capture.name = entry.value("name", std::string());
            capture.time = entry.value("time", std::string());
            capture.bytes = entry.value("bytes", std::uint64_t{0});
            capture.fmt = entry.value("fmt", std::string());
            capture.supported = entry.value("supported", false);
            captures_.push_back(std::move(capture));
        }
    }
    if (root.contains("copied") && root["copied"].is_array()) {
        for (const nlohmann::json& entry : root["copied"]) {
            if (!entry.is_object()) continue;
            const std::string destination = entry.value("dst", std::string());
            if (!destination.empty()) copied_.emplace_back(destination);
        }
    }

    state_ = root.value("ok", false) ? State::Done : State::Failed;
    pending_id_ = -1;
}

void HelperClient::poll() {
    if (state_ != State::Waiting) return;

    std::error_code error;
    const Path response = helper_dir_ / kHelperResponseName;
    if (std::filesystem::exists(response, error)) {
        const std::string text = read_text(response);
        if (response_matches(text)) {
            handle_response(text);
            return;
        }
    }

    if (!alive()) {
        state_ = State::NoHelper;
        error_ = "helper not running";
        return;
    }
    if (std::chrono::steady_clock::now() >= deadline_) {
        state_ = State::Failed;
        error_ = "helper timeout";
    }
}

// PS5_TITLE is the title build's own define; __PROSPERO__ is what the payload
// SDK's clang sets when the core library is cross-compiled for the console, so
// the same file keeps the real socket path in the title and the false path on
// the host.
bool start_helper(const Path& elf) {
#if defined(PS5_TITLE) || defined(__PROSPERO__)
    const std::string image = read_text(elf);
    if (image.empty()) return false;

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    int no_sigpipe = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kElfldrPort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    std::size_t sent = 0;
    while (ok && sent < image.size()) {
        const ssize_t written = ::send(fd, image.data() + sent, image.size() - sent, 0);
        if (written <= 0) {
            ok = false;
            break;
        }
        sent += static_cast<std::size_t>(written);
    }
    if (ok) ::shutdown(fd, SHUT_WR);
    ::close(fd);
    return ok;
#else
    (void)elf;  // the host has no loader on 9021
    return false;
#endif
}
}  // namespace splat
