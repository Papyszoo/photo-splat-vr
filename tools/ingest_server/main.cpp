// splat_ingest_server: manual-testing CLI for the C9 LAN upload page.
//
//   splat_ingest_server --port 8090 --inbox DIR [--state FILE] [--bind ADDR]
//                       [--max-mb N] [--backend NAME]
//
// The server itself is core/ingest/ingest_server. Uploads land in --inbox.
// With --state the process also runs the existing InboxWatcher and JobQueue,
// so an upload appears in /api/queue without the rest of the app. Without it
// the queue endpoint is empty (the real app supplies its own queue snapshot).

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/path.h"
#include "ingest/ingest_server.h"
#include "jobs/inbox_watcher.h"
#include "jobs/job_queue.h"

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop = true; }

void usage() {
    std::fprintf(stderr,
                 "usage: splat_ingest_server --port 8090 --inbox DIR [options]\n"
                 "  --port N          TCP port on the LAN (default 8090)\n"
                 "  --inbox DIR       uploads land here, watched by the app\n"
                 "  --state FILE      optional job queue file; also runs the inbox\n"
                 "                    watcher so uploads show up in /api/queue\n"
                 "  --bind ADDR       bind a private LAN or 127.0.0.1 address\n"
                 "  --max-mb N        per-file upload limit in MiB (default 40)\n"
                 "  --backend NAME    queue backend for watched photos (default fallback)\n");
}

uint64_t parse_positive(const char* text, const char* name) {
    char* end = nullptr;
    const long long value = std::strtoll(text, &end, 10);
    if (end == text || *end != '\0' || value <= 0) {
        std::fprintf(stderr, "splat_ingest_server: %s needs a positive number\n", name);
        std::exit(2);
    }
    return static_cast<uint64_t>(value);
}

}  // namespace

int main(int argc, char** argv) {
    uint16_t port = 8090;
    splat::Path inbox;
    splat::Path state_file;
    std::string bind_address;
    std::string backend = "fallback";
    uint64_t max_mb = 40;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto next = [&](const char* name) -> std::string {
            if (index + 1 >= argc) {
                std::fprintf(stderr, "splat_ingest_server: %s needs a value\n", name);
                std::exit(2);
            }
            return argv[++index];
        };
        if (argument == "-h" || argument == "--help") {
            usage();
            return 0;
        } else if (argument == "--port") {
            port = static_cast<uint16_t>(parse_positive(next("--port").c_str(), "--port"));
        } else if (argument == "--inbox") {
            inbox = next("--inbox");
        } else if (argument == "--state") {
            state_file = next("--state");
        } else if (argument == "--bind") {
            bind_address = next("--bind");
        } else if (argument == "--max-mb") {
            max_mb = parse_positive(next("--max-mb").c_str(), "--max-mb");
        } else if (argument == "--backend") {
            backend = next("--backend");
        } else {
            std::fprintf(stderr, "splat_ingest_server: unexpected argument '%s'\n",
                         argument.c_str());
            usage();
            return 2;
        }
    }

    if (inbox.empty()) {
        usage();
        return 2;
    }
    std::error_code error;
    std::filesystem::create_directories(inbox, error);
    if (error) {
        std::fprintf(stderr, "splat_ingest_server: cannot create inbox %s: %s\n",
                     inbox.string().c_str(), error.message().c_str());
        return 1;
    }

    // Queue state shared with the HTTP worker threads. JobQueue is not
    // thread-safe, so every access goes through this mutex.
    std::mutex queue_mutex;
    std::unique_ptr<splat::JobQueue> queue;
    if (!state_file.empty()) {
        auto opened = splat::JobQueue::open(state_file);
        if (!opened) {
            std::fprintf(stderr, "splat_ingest_server: %s\n", opened.message().c_str());
            return 1;
        }
        queue = std::make_unique<splat::JobQueue>(opened.take());
    }

    splat::IngestServerConfig config;
    config.inbox = inbox;
    config.port = port;
    config.bind_address = bind_address;
    config.max_upload_bytes = max_mb * 1024ull * 1024ull;
    config.queue = [&queue, &queue_mutex]() {
        splat::QueueSnapshot snapshot;
        std::lock_guard<std::mutex> lock(queue_mutex);
        if (!queue) return snapshot;
        for (const splat::Job& job : queue->jobs()) {
            splat::QueueEntry entry;
            entry.id = job.id;
            entry.photo = job.photo.string();
            entry.state = splat::job_state_name(job.state);
            entry.message = job.message;
            entry.seconds_taken = job.seconds_taken;
            snapshot.push_back(std::move(entry));
        }
        return snapshot;
    };

    splat::IngestServer server(std::move(config));
    const splat::Status started = server.start();
    if (!started) {
        std::fprintf(stderr, "splat_ingest_server: %s\n", started.message().c_str());
        return 1;
    }

    std::printf("PIN: %s\n", server.pin().c_str());
    std::printf("Listening on http://%s:%u/\n", server.bind_address().c_str(), server.port());
    std::printf("Inbox: %s\n", inbox.string().c_str());
    if (server.bind_address() == "127.0.0.1") {
        std::printf("No private LAN address found: the page is loopback-only.\n");
        std::printf("Pass --bind <lan-ip> if you know the console's address.\n");
    } else {
        std::printf("LAN page only: no port forwarding, no UPnP; do not expose it to the internet.\n");
    }
    if (queue) {
        std::printf("Queue: %s (backend %s)\n", state_file.string().c_str(), backend.c_str());
    } else {
        std::printf("Queue: no --state file; /api/queue stays empty.\n");
    }
    std::printf("Press Ctrl-C to stop.\n");
    std::fflush(stdout);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // With a queue file, run the real watcher + queue loop: an upload becomes a
    // queued job exactly like a photo copied onto a USB drive.
    std::thread watcher;
    if (queue) {
        watcher = std::thread([&inbox, &queue, &queue_mutex, &backend] {
            splat::InboxWatcher inbox_watcher({inbox});
            while (!g_stop) {
                for (int tick = 0; tick < 10 && !g_stop; ++tick) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (g_stop) break;
                const std::vector<splat::Path> ready = inbox_watcher.poll();
                if (ready.empty()) continue;
                std::lock_guard<std::mutex> lock(queue_mutex);
                for (const splat::Path& photo : ready) {
                    const auto added = queue->enqueue(photo, backend);
                    if (added) {
                        std::printf("queued %s as %s\n", photo.filename().string().c_str(),
                                    added.value().c_str());
                    } else {
                        std::printf("skip %s: %s\n", photo.filename().string().c_str(),
                                    added.message().c_str());
                    }
                    std::fflush(stdout);
                }
            }
        });
    }

    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    g_stop = true;
    server.stop();
    if (watcher.joinable()) watcher.join();
    std::printf("\nstopped\n");
    return 0;
}
