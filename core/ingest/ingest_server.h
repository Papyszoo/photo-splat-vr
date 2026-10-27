#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "common/path.h"
#include "common/status.h"
#include "ingest/pin_gate.h"
#include "ingest/upload_store.h"

namespace httplib {
class Request;
class Response;
class Server;
}  // namespace httplib

namespace splat {

// One row of the queue as the page shows it. The provider decides where the
// data comes from: the app's job runner, or tools/ingest_server reading the
// JobQueue state file. The HTTP layer never owns or mutates the queue.
struct QueueEntry {
    std::string id;
    std::string photo;  // file name or path; only the base name is served
    std::string state;  // queued / running / done / failed / cancelled
    std::string message;
    double seconds_taken = 0.0;
};

using QueueSnapshot = std::vector<QueueEntry>;
// Called from HTTP worker threads on every /api/queue poll, so it must be
// thread-safe and cheap. It only reads queue state; the upload server never
// enqueues (the InboxWatcher does that).
using QueueProvider = std::function<QueueSnapshot()>;

struct IngestServerConfig {
    Path inbox;
    // Empty means: detect the private LAN IPv4 address. If none exists the
    // server binds loopback instead of 0.0.0.0, so it is never exposed.
    std::string bind_address;
    uint16_t port = 8090;  // 0 picks a free port (tests)
    uint64_t max_upload_bytes = UploadStore::kDefaultMaxBytes;
    PinGate::Config pin;
    QueueProvider queue;
};

// C9. HTTP server for the LAN upload page:
//
//   GET  /             embedded page (no external assets)
//   POST /api/login    form field "pin" -> session cookie
//   POST /api/upload   multipart field "photo", JPEG/PNG only
//   GET  /api/queue    queue snapshot as JSON (needs the cookie)
//
// It listens on one LAN address only. There is no UPnP, no port forwarding and
// no internet-facing listener; see core/ingest/README.md.
class IngestServer {
public:
    explicit IngestServer(IngestServerConfig config);
    ~IngestServer();

    IngestServer(const IngestServer&) = delete;
    IngestServer& operator=(const IngestServer&) = delete;

    // Picks the PIN, binds the socket and starts the accept thread. The PIN is
    // available before start() so the TV UI can show it while the port opens.
    Status start();
    void stop();
    bool running() const;

    const std::string& pin() const { return gate_.pin(); }
    const std::string& bind_address() const { return bind_address_; }
    uint16_t port() const { return port_; }

private:
    void install_routes();
    bool authenticated(const httplib::Request& request) const;

    IngestServerConfig config_;
    PinGate gate_;
    UploadStore store_;
    std::unique_ptr<httplib::Server> server_;
    std::thread thread_;
    std::string bind_address_;
    uint16_t port_ = 0;
    std::atomic<bool> started_{false};
};

}  // namespace splat
