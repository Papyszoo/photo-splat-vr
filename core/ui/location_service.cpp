// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/location_service.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <utility>
#include <vector>

namespace splat {

LocationService::LocationService(splat::vfs::LocationStore store,
                                 splat::vfs::FileSystems& file_systems,
                                 std::function<void()> on_changed, Prober prober)
    : store_(std::move(store)), file_systems_(file_systems), on_changed_(std::move(on_changed)),
      prober_(std::move(prober)) {
    for (const splat::vfs::Location& location : store_.locations()) {
        states_[location.id] = "mounting";
    }
    // Register every adapter before the first probe: a location path has to
    // resolve by its id from the moment the service exists, because the queue
    // can start a job (a resumed one at boot) before the mount worker has
    // answered - the console pass saw a job fail with "unknown location
    // \"smb0\"" about a location configured in the same boot. Registration
    // cannot block: the adapter connects on use.
    (void)store_.register_adapters(file_systems_);
    // The console mounts on start: the worker does it while the panel draws its
    // first frame, and a NAS that is off only costs the timeout.
    (void)mount_all();
}

LocationService::~LocationService() {
    stop_.store(true);
    done_.store(true);
    if (worker_.joinable()) worker_.join();
}

LocationView LocationService::view_of(const splat::vfs::Location& location,
                                      const std::string& state) {
    LocationView view;
    view.id = location.id;
    view.name = location.display_name();
    view.protocol = location.protocol == splat::vfs::LocationProtocol::nfs ? "nfs" : "smb";
    view.host = location.host;
    view.port = location.port;
    view.share = location.share;
    view.path = location.path;
    view.user = location.user;
    view.password = location.password;
    view.domain = location.domain;
    view.encrypt = location.encrypt;
    view.photo_source = location.photo_source;
    view.splat_library = location.splat_library;
    view.nfs_version = location.nfs_version;
    view.uid = location.uid;
    view.gid = location.gid;
    view.state = state;
    return view;
}

splat::vfs::Location LocationService::location_of(const LocationView& view) {
    splat::vfs::Location location;
    location.id = view.id;
    location.name = view.name;
    location.protocol = view.protocol == "nfs" ? splat::vfs::LocationProtocol::nfs
                                               : splat::vfs::LocationProtocol::smb;
    location.host = view.host;
    location.port = view.port;
    location.share = view.share;
    location.path = view.path;
    location.user = view.user;
    location.password = view.password;
    location.domain = view.domain.empty() ? std::string("WORKGROUP") : view.domain;
    location.encrypt = view.encrypt;
    location.photo_source = view.photo_source;
    location.splat_library = view.splat_library;
    // Only 3 and 4 exist (nfs_set_version refuses anything else); a stray value
    // from a hand-edited file falls back to the default rather than failing the
    // mount.
    location.nfs_version = view.nfs_version == 4 ? 4 : 3;
    location.uid = view.uid;
    location.gid = view.gid;
    return location;
}

std::vector<LocationView> LocationService::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LocationView> views;
    views.reserve(store_.locations().size());
    for (const splat::vfs::Location& location : store_.locations()) {
        const auto state = states_.find(location.id);
        views.push_back(view_of(location, state == states_.end() ? std::string("mounting")
                                                                 : state->second));
    }
    return views;
}

std::string LocationService::status(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto state = states_.find(id);
    return state == states_.end() ? std::string() : state->second;
}

Status LocationService::add(const LocationView& location, std::string* id) {
    splat::vfs::Location stored = location_of(location);
    std::string assigned;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const splat::Result<std::string> added = store_.add(std::move(stored));
        if (!added.ok()) return Status::error(added.message());
        assigned = added.value();
        states_[assigned] = "mounting";
    }
    if (id != nullptr) *id = assigned;
    notify();
    // Queued behind a mount that is already running, never dropped: the worker
    // takes it as soon as that one answers.
    return mount(assigned);
}

Status LocationService::update(const LocationView& location) {
    splat::vfs::Location stored = location_of(location);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const Status updated = store_.update(stored);
        if (!updated.ok()) return updated;
        states_[location.id] = "mounting";
    }
    notify();
    file_systems_.remove(location.id);  // the old adapter is stale
    return mount(location.id);
}

Status LocationService::remove(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const Status removed = store_.remove(id);
        if (!removed.ok()) return removed;
        states_.erase(id);
    }
    drop_queued(id);  // a mount still waiting for it must not resurrect it
    file_systems_.remove(id);
    notify();
    return Status::success();
}

void LocationService::drop_queued(const std::string& id) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                [&id](const splat::vfs::Location& location) {
                                    return location.id == id;
                                }),
                 queue_.end());
}

Status LocationService::mount_all() {
    std::vector<splat::vfs::Location> locations;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        locations = store_.locations();
        for (const splat::vfs::Location& location : locations) {
            states_[location.id] = "mounting";
        }
    }
    return start_worker(std::move(locations));
}

Status LocationService::mount(const std::string& id) {
    splat::vfs::Location location;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const splat::vfs::Location* found = store_.find(id);
        if (found == nullptr) return Status::error("no such location: " + id);
        location = *found;
        states_[id] = "mounting";
    }
    return start_worker({std::move(location)});
}

void LocationService::unmount(const std::string& id) {
    drop_queued(id);
    file_systems_.remove(id);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        states_[id] = "offline: not mounted";
    }
    notify();
}

bool LocationService::busy() const {
    if (!done_.load()) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& state : states_) {
        if (state.second == "mounting") return true;
    }
    return false;
}

Status LocationService::start_worker(std::vector<splat::vfs::Location> locations) {
    // A mount request that arrives while one runs is queued, not refused: the
    // running worker drains the queue, so a location added (or an update saved)
    // during the start-up mount reaches "ready" instead of sitting on
    // "mounting" for the rest of the session.
    bool start = false;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        for (splat::vfs::Location& location : locations) {
            queue_.push_back(std::move(location));
        }
        if (queue_.empty()) return Status::success();
        if (done_.load()) {
            done_.store(false);
            start = true;
        }
    }
    if (!start) return Status::success();  // the running worker will take it
    // The previous worker has published done_ and left its loop; join it before
    // replacing the thread.
    if (worker_.joinable()) worker_.join();
    worker_ = std::thread(&LocationService::worker_main, this);
    return Status::success();
}

void LocationService::worker_main() {
    for (;;) {
        splat::vfs::Location location;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (stop_.load()) {
                queue_.clear();
                done_.store(true);
                return;
            }
            if (queue_.empty()) {
                // Published under the queue lock: a start_worker() that pushes
                // after this point sees done_ and starts a new thread.
                done_.store(true);
                return;
            }
            location = std::move(queue_.front());
            queue_.erase(queue_.begin());
        }
        const std::string state = probe_one(location);
        std::lock_guard<std::mutex> lock(result_mutex_);
        results_[location.id] = state;
    }
}

std::string LocationService::probe_one(const splat::vfs::Location& location) {
    // The elapsed time goes into the state: the console checklist promises a
    // dead share is reported "within 7 s", and only the mount can measure it.
    // The console run asserts it instead of trusting the whole case's bound
    // (console pass 3, T5).
    const auto started = std::chrono::steady_clock::now();
    std::string state = prober_ ? prober_(location, file_systems_)
                                : splat::vfs::LocationStore::mount_one(file_systems_, location);
    if (state.compare(0, 8, "offline:") != 0) return state;
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    char prefix[48];
    std::snprintf(prefix, sizeof(prefix), "offline after %.1f s:", seconds);
    return std::string(prefix) + state.substr(8);
}

void LocationService::check_health() {
    // A location that was ready and whose adapter has since dropped its
    // connection (one server reply libsmb2 could not read) must stop saying
    // ready: the user would keep pressing a row the app calls healthy (smb-e2e,
    // BUG 1). The adapter reconnects on its next use, and the row goes back to
    // ready when that works; the mount and the Test button do it at once.
    std::vector<std::string> watched;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& state : states_) {
            if (state.second == "ready" || health_offline_.count(state.first) != 0) {
                watched.push_back(state.first);
            }
        }
    }
    for (const std::string& id : watched) {
        const std::shared_ptr<splat::vfs::IFileSystem> mounted = file_systems_.find(id);
        const std::string health = mounted != nullptr ? mounted->connection_error() : std::string();
        std::lock_guard<std::mutex> lock(mutex_);
        const auto state = states_.find(id);
        if (state == states_.end()) continue;
        if (!health.empty()) {
            state->second = "offline: " + health;
            health_offline_.insert(id);
        } else if (health_offline_.erase(id) != 0) {
            state->second = "ready";
        }
    }
}

void LocationService::join_finished() {
    if (done_.load() && worker_.joinable()) worker_.join();
}

void LocationService::tick(double /*dt*/) {
    check_health();
    if (!done_.load()) return;
    join_finished();
    std::map<std::string, std::string> results;
    {
        std::lock_guard<std::mutex> lock(result_mutex_);
        results.swap(results_);
    }
    if (results.empty()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& result : results) {
            states_[result.first] = result.second;
        }
    }
    notify();
}

void LocationService::notify() {
    if (on_changed_) on_changed_();
}

}  // namespace splat
