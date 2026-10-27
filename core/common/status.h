#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace splat {

// Minimal error type. Components return Status or Result<T>; exceptions are
// reserved for programming errors (out-of-range access), not for I/O failures.
class Status {
public:
    Status() = default;

    static Status success() { return Status(); }
    static Status error(std::string message) { return Status(std::move(message)); }

    bool ok() const { return message_.empty(); }
    explicit operator bool() const { return ok(); }
    const std::string& message() const { return message_; }

private:
    explicit Status(std::string message) : message_(std::move(message)) {}

    std::string message_;
};

// A value or an error message. T must be default-constructible.
template <typename T>
class Result {
public:
    static Result success(T value) {
        Result result;
        result.ok_ = true;
        result.value_ = std::move(value);
        return result;
    }

    static Result failure(std::string message) {
        Result result;
        result.ok_ = false;
        result.message_ = std::move(message);
        return result;
    }

    bool ok() const { return ok_; }
    explicit operator bool() const { return ok_; }
    const std::string& message() const { return message_; }

    T& value() {
        if (!ok_) throw std::logic_error("Result::value() on an error: " + message_);
        return value_;
    }
    const T& value() const {
        if (!ok_) throw std::logic_error("Result::value() on an error: " + message_);
        return value_;
    }
    T take() {
        if (!ok_) throw std::logic_error("Result::take() on an error: " + message_);
        return std::move(value_);
    }

    T value_or(T fallback) const { return ok_ ? value_ : std::move(fallback); }

private:
    bool ok_ = false;
    T value_{};
    std::string message_;
};

}  // namespace splat
