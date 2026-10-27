#include "ingest/ingest_server.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <string>

#include <arpa/inet.h>

#include <nlohmann/json.hpp>

#include "httplib/httplib.h"
#include "ingest/ingest_page.h"
#include "ingest/lan_address.h"

namespace splat {
namespace {

constexpr const char* kSessionCookie = "splat_session";

std::string session_token(const httplib::Request& request) {
    const std::string header = request.get_header_value("Cookie");
    size_t start = 0;
    while (start < header.size()) {
        size_t end = header.find(';', start);
        if (end == std::string::npos) end = header.size();
        size_t first = start;
        while (first < end && (header[first] == ' ' || header[first] == '\t')) ++first;
        const std::string part = header.substr(first, end - first);
        const std::string prefix = std::string(kSessionCookie) + "=";
        if (part.compare(0, prefix.size(), prefix) == 0) {
            return part.substr(prefix.size());
        }
        start = end + 1;
    }
    return {};
}

void send_json(httplib::Response& response, int status, const nlohmann::json& body) {
    response.status = status;
    response.set_header("Cache-Control", "no-store");
    response.set_content(body.dump(), "application/json; charset=utf-8");
}

void send_error(httplib::Response& response, int status, const std::string& message) {
    send_json(response, status, {{"ok", false}, {"error", message}});
}

void serve_page(httplib::Response& response) {
    response.set_header("Cache-Control", "no-store");
    response.set_header("X-Content-Type-Options", "nosniff");
    response.set_content(kIngestPage, "text/html; charset=utf-8");
}

std::string base_name(const std::string& path) {
    const std::string name = std::filesystem::path(path).filename().string();
    return name.empty() ? path : name;
}

// Only loopback and RFC 1918 addresses are allowed. 0.0.0.0, a public address
// or a hostname is refused, so an operator cannot expose the upload page by
// mistake.
bool is_lan_or_loopback(const std::string& address) {
    in_addr parsed{};
    if (::inet_pton(AF_INET, address.c_str(), &parsed) != 1) return false;
    const auto* bytes = reinterpret_cast<const unsigned char*>(&parsed.s_addr);
    if (bytes[0] == 127) return true;
    if (bytes[0] == 10) return true;
    if (bytes[0] == 192 && bytes[1] == 168) return true;
    if (bytes[0] == 172 && bytes[1] >= 16 && bytes[1] <= 31) return true;
    return false;
}

}  // namespace

IngestServer::IngestServer(IngestServerConfig config)
    : config_(std::move(config)),
      gate_(PinGate::generate_pin(), config_.pin),
      store_(config_.inbox, config_.max_upload_bytes) {}

IngestServer::~IngestServer() { stop(); }

Status IngestServer::start() {
    if (config_.inbox.empty()) return Status::error("the inbox directory is required");
    if (started_.exchange(true)) return Status::error("the ingest server is already running");

    bind_address_ = config_.bind_address;
    if (bind_address_.empty()) bind_address_ = detect_lan_address();
    // No private LAN address: loopback keeps the page local instead of
    // listening on every interface.
    if (bind_address_.empty()) bind_address_ = "127.0.0.1";
    if (!is_lan_or_loopback(bind_address_)) {
        started_ = false;
        return Status::error("refusing to bind " + bind_address_ +
                             ": the upload page is LAN-only (private IPv4 or 127.0.0.1)");
    }

    server_ = std::make_unique<httplib::Server>();
    const uint64_t payload_limit =
        std::min<uint64_t>(config_.max_upload_bytes + 1024ull * 1024ull,
                           static_cast<uint64_t>(std::numeric_limits<size_t>::max()));
    server_->set_payload_max_length(static_cast<size_t>(payload_limit));
    server_->set_read_timeout(60);
    server_->set_write_timeout(60);
    server_->set_keep_alive_max_count(50);
    install_routes();

    if (config_.port == 0) {
        const int port = server_->bind_to_any_port(bind_address_);
        if (port <= 0) {
            server_.reset();
            started_ = false;
            return Status::error("cannot bind " + bind_address_ + " on any port");
        }
        port_ = static_cast<uint16_t>(port);
    } else {
        if (!server_->bind_to_port(bind_address_, config_.port)) {
            server_.reset();
            started_ = false;
            return Status::error("cannot bind " + bind_address_ + ":" +
                                 std::to_string(config_.port));
        }
        port_ = config_.port;
    }

    thread_ = std::thread([this] { server_->listen_after_bind(); });
    server_->wait_until_ready();
    if (!server_->is_running()) {
        stop();
        return Status::error("the ingest server stopped while starting");
    }
    return Status::success();
}

void IngestServer::stop() {
    if (server_) server_->stop();
    if (thread_.joinable()) thread_.join();
    started_ = false;
}

bool IngestServer::running() const { return server_ != nullptr && server_->is_running(); }

bool IngestServer::authenticated(const httplib::Request& request) const {
    return gate_.is_session(session_token(request));
}

void IngestServer::install_routes() {
    server_->Get("/", [](const httplib::Request&, httplib::Response& response) {
        serve_page(response);
    });
    server_->Get("/index.html", [](const httplib::Request&, httplib::Response& response) {
        serve_page(response);
    });
    server_->Get("/favicon.ico", [](const httplib::Request&, httplib::Response& response) {
        response.status = 204;
    });

    server_->Post("/api/login", [this](const httplib::Request& request,
                                       httplib::Response& response) {
        if (request.get_param_value_count("pin") == 0) {
            send_error(response, 400, "missing pin");
            return;
        }
        const PinGate::LoginResult result =
            gate_.login(request.remote_addr, request.get_param_value("pin"));
        switch (result.outcome) {
            case PinGate::Outcome::Accepted: {
                const long long seconds = gate_.config().session_ttl.count();
                response.set_header("Set-Cookie",
                                    std::string(kSessionCookie) + "=" + result.token +
                                        "; Path=/; HttpOnly; SameSite=Lax; Max-Age=" +
                                        std::to_string(seconds));
                send_json(response, 200, {{"ok", true}});
                return;
            }
            case PinGate::Outcome::WrongPin:
                send_error(response, 401, "wrong PIN");
                return;
            case PinGate::Outcome::LockedOut:
                response.set_header(
                    "Retry-After",
                    std::to_string(std::max<long long>(1, result.retry_after.count())));
                send_error(response, 429, "too many attempts; wait before trying again");
                return;
        }
    });

    server_->Post("/api/upload", [this](const httplib::Request& request,
                                        httplib::Response& response) {
        if (!authenticated(request)) {
            send_error(response, 401, "enter the PIN first");
            return;
        }
        if (!request.has_file("photo")) {
            send_error(response, 400, "missing multipart field 'photo'");
            return;
        }
        const httplib::MultipartFormData file = request.get_file_value("photo");
        if (file.content.size() > config_.max_upload_bytes) {
            send_error(response, 413, "image is larger than the upload limit");
            return;
        }
        const Result<std::string> stored = store_.store(file.filename, file.content);
        if (!stored) {
            send_error(response, 400, stored.message());
            return;
        }
        send_json(response, 200,
                  {{"ok", true}, {"file", stored.value()}, {"bytes", file.content.size()}});
    });

    server_->Get("/api/queue", [this](const httplib::Request& request,
                                      httplib::Response& response) {
        if (!authenticated(request)) {
            send_error(response, 401, "enter the PIN first");
            return;
        }

        nlohmann::json jobs = nlohmann::json::array();
        nlohmann::json counts = {{"queued", 0}, {"running", 0}, {"done", 0},
                                 {"failed", 0}, {"cancelled", 0}};
        try {
            const QueueSnapshot snapshot = config_.queue ? config_.queue() : QueueSnapshot{};
            for (const QueueEntry& entry : snapshot) {
                jobs.push_back({{"id", entry.id},
                                {"photo", base_name(entry.photo)},
                                {"state", entry.state},
                                {"message", entry.message},
                                {"seconds_taken", entry.seconds_taken}});
                counts[entry.state] = counts.value(entry.state, 0) + 1;
            }
        } catch (const std::exception& error) {
            send_error(response, 500, std::string("cannot read the queue: ") + error.what());
            return;
        }
        send_json(response, 200, {{"ok", true}, {"jobs", jobs}, {"counts", counts}});
    });
}

}  // namespace splat
