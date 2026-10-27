#include <doctest/doctest.h>

#include <chrono>
#include <set>
#include <string>

#include "ingest/pin_gate.h"

namespace {

bool all_digits(const std::string& text) {
    if (text.empty()) return false;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

bool all_hex(const std::string& text) {
    if (text.empty()) return false;
    for (const char c : text) {
        const bool digit = c >= '0' && c <= '9';
        const bool lower = c >= 'a' && c <= 'f';
        if (!digit && !lower) return false;
    }
    return true;
}

}  // namespace

TEST_CASE("the generated PIN is always six digits without a leading zero") {
    for (int i = 0; i < 50; ++i) {
        const std::string pin = splat::PinGate::generate_pin();
        CHECK(pin.size() == 6);
        CHECK(all_digits(pin));
        CHECK(pin.front() != '0');
    }
}

TEST_CASE("generated PINs come from the CSPRNG and are not a repeating sequence") {
    // 6-digit PINs have a small space, so this only catches a generator stuck
    // on one value (a seeded mt19937 per call is a different bug, not visible
    // from the outside). The token test below covers the random source more
    // strictly.
    std::set<std::string> seen;
    for (int i = 0; i < 100; ++i) seen.insert(splat::PinGate::generate_pin());
    // 100 draws from 900 000 PINs: far more than 50 distinct is certain unless
    // the generator is stuck or cycling over a handful of values.
    CHECK(seen.size() > 50);
}

TEST_CASE("a correct PIN creates a session and unknown tokens are rejected") {
    splat::PinGate gate("123456");
    CHECK(gate.pin() == "123456");

    CHECK_FALSE(gate.is_session(""));
    CHECK_FALSE(gate.is_session("deadbeef"));

    const splat::PinGate::LoginResult wrong = gate.login("phone", "000000");
    CHECK(wrong.outcome == splat::PinGate::Outcome::WrongPin);
    CHECK(wrong.token.empty());

    const splat::PinGate::LoginResult accepted = gate.login("phone", "123456");
    CHECK(accepted.outcome == splat::PinGate::Outcome::Accepted);
    CHECK(accepted.token.size() == 32);   // 128 random bits, hex
    CHECK(all_hex(accepted.token));
    CHECK(gate.is_session(accepted.token));
    CHECK_FALSE(gate.is_session(accepted.token + "0"));
    CHECK_FALSE(gate.is_session(accepted.token.substr(0, 31)));
    // A token that shares a prefix must not pass either.
    std::string prefix = accepted.token;
    prefix.back() = prefix.back() == '0' ? '1' : '0';
    CHECK_FALSE(gate.is_session(prefix));
}

TEST_CASE("every login gets a fresh 128-bit session token") {
    splat::PinGate gate("123456");
    std::set<std::string> tokens;
    for (int i = 0; i < 64; ++i) {
        const splat::PinGate::LoginResult result = gate.login("phone", "123456");
        REQUIRE(result.outcome == splat::PinGate::Outcome::Accepted);
        CHECK(result.token.size() == 32);
        CHECK(all_hex(result.token));
        CHECK(tokens.insert(result.token).second);
        CHECK(gate.is_session(result.token));
    }
}

TEST_CASE("five wrong PINs lock the address out for sixty seconds") {
    auto now = std::chrono::steady_clock::time_point{} + std::chrono::seconds(1000);
    splat::PinGate gate("123456", {}, [&now] { return now; });

    for (int attempt = 1; attempt <= 4; ++attempt) {
        const splat::PinGate::LoginResult result = gate.login("192.168.1.20", "999999");
        CHECK(result.outcome == splat::PinGate::Outcome::WrongPin);
    }

    const splat::PinGate::LoginResult fifth = gate.login("192.168.1.20", "999999");
    CHECK(fifth.outcome == splat::PinGate::Outcome::LockedOut);
    CHECK(fifth.retry_after == std::chrono::seconds(60));

    // The right PIN does not bypass the lockout.
    const splat::PinGate::LoginResult locked_correct = gate.login("192.168.1.20", "123456");
    CHECK(locked_correct.outcome == splat::PinGate::Outcome::LockedOut);
    CHECK_FALSE(gate.is_session(locked_correct.token));

    // Another phone is unaffected.
    const splat::PinGate::LoginResult other = gate.login("192.168.1.21", "123456");
    CHECK(other.outcome == splat::PinGate::Outcome::Accepted);

    now += std::chrono::seconds(59);
    CHECK(gate.login("192.168.1.20", "123456").outcome ==
          splat::PinGate::Outcome::LockedOut);

    now += std::chrono::seconds(2);
    const splat::PinGate::LoginResult after = gate.login("192.168.1.20", "123456");
    CHECK(after.outcome == splat::PinGate::Outcome::Accepted);
}

TEST_CASE("twenty wrong PINs from any clients trip a global sixty-second lockout") {
    auto now = std::chrono::steady_clock::time_point{};
    splat::PinGate gate("123456", {}, [&now] { return now; });

    // Nineteen wrong attempts spread over nineteen phones: under every
    // per-address limit and under the global cap.
    for (int i = 0; i < 19; ++i) {
        const std::string client = "192.168.1." + std::to_string(10 + i);
        CHECK(gate.login(client, "000000").outcome == splat::PinGate::Outcome::WrongPin);
    }
    // One more phone can still get in, and that success does not erase the
    // global record.
    CHECK(gate.login("192.168.1.99", "123456").outcome == splat::PinGate::Outcome::Accepted);

    // The twentieth wrong PIN, from yet another phone, locks everybody out.
    const splat::PinGate::LoginResult twentieth = gate.login("192.168.1.100", "999999");
    CHECK(twentieth.outcome == splat::PinGate::Outcome::LockedOut);
    CHECK(twentieth.retry_after == std::chrono::seconds(60));

    // No client can log in, not even one that never failed and knows the PIN.
    const splat::PinGate::LoginResult everywhere = gate.login("192.168.1.101", "123456");
    CHECK(everywhere.outcome == splat::PinGate::Outcome::LockedOut);
    CHECK_FALSE(gate.is_session(everywhere.token));

    now += std::chrono::seconds(30);
    const splat::PinGate::LoginResult halfway = gate.login("192.168.1.101", "123456");
    CHECK(halfway.outcome == splat::PinGate::Outcome::LockedOut);
    CHECK(halfway.retry_after == std::chrono::seconds(30));

    // After the global lockout and the rolling window both pass, a fresh phone
    // is accepted again.
    now += std::chrono::seconds(31);
    CHECK(gate.login("192.168.1.101", "123456").outcome == splat::PinGate::Outcome::Accepted);
}

TEST_CASE("the global failure window is rolling, not a burst counter") {
    auto now = std::chrono::steady_clock::time_point{};
    splat::PinGate gate("123456", {}, [&now] { return now; });

    for (int i = 0; i < 10; ++i) {
        CHECK(gate.login("10.0.0." + std::to_string(i), "000000").outcome ==
              splat::PinGate::Outcome::WrongPin);
    }
    // The first ten age out of the one-minute window before the next ten.
    now += std::chrono::seconds(61);
    for (int i = 0; i < 10; ++i) {
        CHECK(gate.login("10.0.1." + std::to_string(i), "000000").outcome ==
              splat::PinGate::Outcome::WrongPin);
    }
    CHECK(gate.login("10.0.2.1", "123456").outcome == splat::PinGate::Outcome::Accepted);
}

TEST_CASE("a correct PIN clears only that phone's failed attempts") {
    auto now = std::chrono::steady_clock::time_point{};
    splat::PinGate gate("123456", {}, [&now] { return now; });

    for (int i = 0; i < 4; ++i) {
        CHECK(gate.login("phone-a", "000000").outcome == splat::PinGate::Outcome::WrongPin);
    }
    CHECK(gate.login("phone-a", "123456").outcome == splat::PinGate::Outcome::Accepted);
    // The reset really happened: four more failures do not reach five.
    for (int i = 0; i < 4; ++i) {
        CHECK(gate.login("phone-a", "000000").outcome == splat::PinGate::Outcome::WrongPin);
    }
    CHECK(gate.login("phone-a", "123456").outcome == splat::PinGate::Outcome::Accepted);
}

TEST_CASE("sessions expire and a new PIN drops the old ones") {
    auto now = std::chrono::steady_clock::time_point{};
    splat::PinGate::Config config;
    config.session_ttl = std::chrono::hours(12);
    splat::PinGate gate("123456", config, [&now] { return now; });

    const splat::PinGate::LoginResult accepted = gate.login("phone", "123456");
    REQUIRE(accepted.outcome == splat::PinGate::Outcome::Accepted);
    CHECK(gate.is_session(accepted.token));

    now += std::chrono::hours(11);
    CHECK(gate.is_session(accepted.token));
    now += std::chrono::hours(2);
    CHECK_FALSE(gate.is_session(accepted.token));

    const splat::PinGate::LoginResult second = gate.login("phone", "123456");
    REQUIRE(second.outcome == splat::PinGate::Outcome::Accepted);
    gate.reset("654321");
    CHECK(gate.pin() == "654321");
    CHECK_FALSE(gate.is_session(second.token));
    CHECK(gate.login("phone", "123456").outcome == splat::PinGate::Outcome::WrongPin);
    CHECK(gate.login("phone", "654321").outcome == splat::PinGate::Outcome::Accepted);
}
