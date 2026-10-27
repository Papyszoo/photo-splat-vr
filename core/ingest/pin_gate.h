#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace splat {

// C9. The 6-digit PIN shown on the TV and the cookies a phone gets after
// typing it. Wrong PINs are rate limited per client address: after
// `max_attempts` failures that address is locked out for `lockout`, and even
// the right PIN is refused until the lockout expires.
//
// A second, global cap stops a whole LAN from multiplying the per-address rate:
// once `global_max_attempts` wrong PINs have arrived from any clients within
// `global_window`, every client is locked out for `global_lockout` (a
// deliberate trade: an attacker can deny logins for a minute, but cannot grind
// through the six-digit space).
//
// Time comes from an injectable steady clock so the lockout can be tested
// without sleeping.
class PinGate {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    struct Config {
        int max_attempts = 5;
        std::chrono::seconds lockout{60};
        std::chrono::seconds session_ttl{std::chrono::hours(12)};
        // Global cap across all clients: 20 wrong PINs in a rolling minute
        // lock everybody out for a minute.
        int global_max_attempts = 20;
        std::chrono::seconds global_window{60};
        std::chrono::seconds global_lockout{60};
    };

    enum class Outcome { Accepted, WrongPin, LockedOut };

    struct LoginResult {
        Outcome outcome = Outcome::WrongPin;
        std::string token;                    // set when Accepted
        std::chrono::seconds retry_after{0};  // set when LockedOut
    };

    // `pin` must be what the UI shows; generate_pin() produces a random one.
    explicit PinGate(std::string pin);
    explicit PinGate(std::string pin, Config config, Clock clock = {});

    // Six random decimal digits from the system CSPRNG, never with a leading
    // zero so the TV can show exactly six characters.
    static std::string generate_pin();

    const std::string& pin() const { return pin_; }
    const Config& config() const { return config_; }

    // Checks a PIN from `client` (normally the remote address) and, on success,
    // creates a session token. Thread-safe.
    LoginResult login(const std::string& client, const std::string& pin);

    // True while `token` is a live session. Expired tokens are dropped.
    bool is_session(const std::string& token) const;

    // Drops every session and rate-limit entry (a new PIN, e.g. on restart).
    void reset(std::string pin);

private:
    using TimePoint = std::chrono::steady_clock::time_point;

    struct Session {
        std::string token;
        TimePoint expires;
    };

    // 128 random bits from the system CSPRNG as 32 lowercase hex characters.
    static std::string generate_token();
    TimePoint now() const;

    mutable std::mutex mutex_;
    std::string pin_;
    Config config_;
    Clock clock_;
    std::map<std::string, int> failures_;
    std::map<std::string, TimePoint> locked_until_;
    // A vector, not a map: is_session() compares every live token in constant
    // time instead of letting a container's key compare leak a prefix.
    std::vector<Session> sessions_;
    std::deque<TimePoint> global_failures_;
    bool global_locked_ = false;
    TimePoint global_locked_until_{};
};

}  // namespace splat
