// ILocationService over core/vfs's LocationStore (L3-C).
//
// The store file is small; the mount is not: libsmb2 blocks on a connect and a
// switched-off NAS answers only when its timeout fires. So add/update/remove
// touch the file on the UI thread and the probing happens on one worker thread
// started by mount(), mount_all() or the constructor; tick() joins it when it
// has finished and publishes the per-location state ("ready", "offline: ..."),
// which is what the Locations screen draws.
//
// Only one mount runs at a time: a second request while one is in flight joins
// the worker's queue and is mounted right after it, so adding a location (or a
// start-up mount that is still timing out) can never leave a row on "mounting"
// for the rest of the session.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ui/services.h"
#include "vfs/io.h"
#include "vfs/location.h"

namespace splat {

class LocationService : public ILocationService {
public:
    // `store` is moved in: the service owns it and saves through it. Nothing
    // is mounted until mount_all()/mount() runs, except that the constructor
    // starts mount_all() on the worker (the console mounts on start).
    // `prober` is the mount itself; empty means LocationStore::mount_one().
    // A test passes its own to hold a mount open without a NAS.
    using Prober = std::function<std::string(const splat::vfs::Location&,
                                             splat::vfs::FileSystems&)>;
    LocationService(splat::vfs::LocationStore store, splat::vfs::FileSystems& file_systems,
                    std::function<void()> on_changed = {})
        : LocationService(std::move(store), file_systems, std::move(on_changed), Prober{}) {}
    LocationService(splat::vfs::LocationStore store, splat::vfs::FileSystems& file_systems,
                    std::function<void()> on_changed, Prober prober);
    ~LocationService() override;

    LocationService(const LocationService&) = delete;
    LocationService& operator=(const LocationService&) = delete;

    std::vector<LocationView> list() const override;
    std::string status(const std::string& id) const override;
    Status add(const LocationView& location, std::string* id = nullptr) override;
    Status update(const LocationView& location) override;
    Status remove(const std::string& id) override;
    Status mount(const std::string& id) override;
    void unmount(const std::string& id) override;
    bool busy() const override;
    void tick(double dt) override;

    // Mounts every configured location, on the worker. Called once by the
    // constructor; exposed for a host that builds the service before it wants
    // the network up.
    Status mount_all();

    const splat::vfs::LocationStore& store() const { return store_; }

private:
    static LocationView view_of(const splat::vfs::Location& location,
                                const std::string& state);
    static splat::vfs::Location location_of(const LocationView& view);
    Status start_worker(std::vector<splat::vfs::Location> locations);
    void worker_main();
    void drop_queued(const std::string& id);
    void join_finished();
    void notify();

    // A location whose adapter dropped its connection is shown offline until
    // it works again; see the comment in location_service.cpp.
    void check_health();
    std::string probe_one(const splat::vfs::Location& location);

    splat::vfs::LocationStore store_;
    splat::vfs::FileSystems& file_systems_;
    std::function<void()> on_changed_;
    Prober prober_;

    mutable std::mutex mutex_;  // store_ and states_ both live under it
    std::map<std::string, std::string> states_;

    // What the worker still has to mount, and the answers tick() publishes.
    // queue_mutex_ also guards done_ on the worker's exit path, so a request
    // pushed exactly as the worker empties cannot be left behind.
    std::mutex queue_mutex_;
    std::vector<splat::vfs::Location> queue_;
    mutable std::mutex result_mutex_;
    std::map<std::string, std::string> results_;
    std::atomic<bool> done_{true};
    std::atomic<bool> stop_{false};
    // The ids check_health() marked offline (under mutex_), so a recovered
    // adapter can put its row back to ready.
    std::set<std::string> health_offline_;

    std::thread worker_;
};

}  // namespace splat
