// SPDX-License-Identifier: GPL-3.0-or-later

#include "vfs/net_state.h"

#include <mutex>
#include <string>
#include <utility>

namespace splat::vfs {
namespace {

std::mutex g_mutex;
std::string g_error;
std::function<void()> g_retry;

}  // namespace

std::string net_stack_error() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_error;
}

void set_net_stack_error(std::string error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_error = std::move(error);
}

void set_net_stack_retry(std::function<void()> retry) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_retry = std::move(retry);
}

bool retry_net_stack() {
    // The hook is copied and the lock released before it runs: it calls
    // set_net_stack_error() itself (through the viewer's ensure_net_init), and
    // a recursive lock here would deadlock the mount worker.
    std::function<void()> retry;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        retry = g_retry;
    }
    if (!retry) return false;
    retry();
    return net_stack_error().empty();
}

}  // namespace splat::vfs
