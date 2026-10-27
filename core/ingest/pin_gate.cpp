#include "ingest/pin_gate.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__linux__)
#include <sys/random.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__DragonFly__) || \
    defined(__OpenBSD__) || defined(__NetBSD__)
#include <stdlib.h>  // arc4random_buf
#endif

namespace splat {
namespace {

// The system CSPRNG: arc4random on macOS/FreeBSD (and so the PS5 payload
// build), getrandom on Linux with /dev/urandom as the fallback. A failure is
// fatal on purpose: a PIN or session token from a weak source would silently
// undo the lockout and the cookie.
void secure_random(void* out, size_t bytes) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__DragonFly__) || \
    defined(__OpenBSD__) || defined(__NetBSD__)
    arc4random_buf(out, bytes);
    return;
#elif defined(__linux__)
    auto* p = static_cast<uint8_t*>(out);
    size_t got = 0;
    while (got < bytes) {
        const ssize_t n = ::getrandom(p + got, bytes - got, 0);
        if (n > 0) {
            got += static_cast<size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    if (got == bytes) return;
#endif
    FILE* f = std::fopen("/dev/urandom", "rb");
    if (f == nullptr) std::abort();
    const size_t read = std::fread(out, 1, bytes, f);
    std::fclose(f);
    if (read != bytes) std::abort();
}

// One uniform decimal digit in [0, 9] by rejection sampling (250..255 dropped)
// so no digit is more likely than another.
int random_digit() {
    for (;;) {
        uint8_t byte = 0;
        secure_random(&byte, sizeof byte);
        if (byte < 250) return byte % 10;
    }
}

// A digit in [1, 9], for the first position (the TV shows six characters and
// the input is a number).
int random_nonzero_digit() {
    for (;;) {
        uint8_t byte = 0;
        secure_random(&byte, sizeof byte);
        if (byte < 252) return 1 + byte % 9;
    }
}

// Compares every byte even when the lengths differ. The length itself is not a
// secret (all tokens are 32 characters), so the sizes are only folded into the
// result.
bool constant_time_equal(const std::string& a, const std::string& b) {
    unsigned char difference = a.size() == b.size() ? 0 : 1;
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        difference |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return difference == 0;
}

}  // namespace

PinGate::PinGate(std::string pin) : PinGate(std::move(pin), Config{}, Clock{}) {}

PinGate::PinGate(std::string pin, Config config, Clock clock)
    : pin_(std::move(pin)), config_(config), clock_(std::move(clock)) {
    if (!clock_) {
        clock_ = [] { return std::chrono::steady_clock::now(); };
    }
}

std::string PinGate::generate_pin() {
    std::string pin;
    pin.reserve(6);
    pin.push_back(static_cast<char>('0' + random_nonzero_digit()));
    for (int i = 1; i < 6; ++i) pin.push_back(static_cast<char>('0' + random_digit()));
    return pin;
}

std::string PinGate::generate_token() {
    uint8_t bytes[16];
    secure_random(bytes, sizeof bytes);
    static const char kHex[] = "0123456789abcdef";
    std::string token(32, '0');
    for (size_t i = 0; i < sizeof bytes; ++i) {
        token[2 * i] = kHex[bytes[i] >> 4];
        token[2 * i + 1] = kHex[bytes[i] & 0x0f];
    }
    return token;
}

PinGate::TimePoint PinGate::now() const { return clock_(); }

PinGate::LoginResult PinGate::login(const std::string& client, const std::string& pin) {
    std::lock_guard<std::mutex> lock(mutex_);
    const TimePoint current = now();

    // Drop expired sessions so a long-running server does not grow unbounded.
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (it->expires <= current) {
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }

    // Global cap first: while it is on, even the right PIN waits.
    if (global_locked_) {
        if (global_locked_until_ > current) {
            const auto remaining =
                std::chrono::ceil<std::chrono::seconds>(global_locked_until_ - current);
            return {Outcome::LockedOut, "", remaining};
        }
        global_locked_ = false;
    }
    while (!global_failures_.empty() &&
           current - global_failures_.front() >= config_.global_window) {
        global_failures_.pop_front();
    }

    const auto locked = locked_until_.find(client);
    if (locked != locked_until_.end()) {
        if (locked->second > current) {
            const auto remaining =
                std::chrono::ceil<std::chrono::seconds>(locked->second - current);
            return {Outcome::LockedOut, "", remaining};
        }
        locked_until_.erase(locked);
        failures_.erase(client);
    }

    if (!constant_time_equal(pin, pin_)) {
        global_failures_.push_back(current);
        int& failures = failures_[client];
        ++failures;

        // The global cap trips on the attempt that reaches it, whatever the
        // client was doing before; it counts wrong PINs, not clients.
        if (static_cast<int>(global_failures_.size()) >= config_.global_max_attempts) {
            global_locked_ = true;
            global_locked_until_ = current + config_.global_lockout;
            return {Outcome::LockedOut, "", config_.global_lockout};
        }
        if (failures >= config_.max_attempts) {
            failures_.erase(client);
            locked_until_[client] = current + config_.lockout;
            return {Outcome::LockedOut, "", config_.lockout};
        }
        return {Outcome::WrongPin, "", {}};
    }

    // A correct PIN clears earlier failures for this client, but not the
    // global record of wrong attempts in the current window.
    failures_.erase(client);
    const std::string token = generate_token();
    sessions_.push_back({token, current + config_.session_ttl});
    return {Outcome::Accepted, token, {}};
}

bool PinGate::is_session(const std::string& token) const {
    if (token.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const TimePoint current = now();
    // Compare against every live token without an early exit, so the lookup
    // takes the same time whatever the guess.
    unsigned char found = 0;
    for (const Session& session : sessions_) {
        const bool live = session.expires > current;
        found |= static_cast<unsigned char>(constant_time_equal(token, session.token) && live);
    }
    return found != 0;
}

void PinGate::reset(std::string pin) {
    std::lock_guard<std::mutex> lock(mutex_);
    pin_ = std::move(pin);
    failures_.clear();
    locked_until_.clear();
    sessions_.clear();
    global_failures_.clear();
    global_locked_ = false;
}

}  // namespace splat
