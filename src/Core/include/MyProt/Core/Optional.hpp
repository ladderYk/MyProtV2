// src/Core/include/MyProt/Core/Optional.hpp
// In-house Optional<T> container - C++11 compatible, replaces std::optional (not supported by v140)
//
// History:
//   2026-08-29 briefly withdrew in favor of std::optional; at compile time the vcxproj v140/C++11 had no <optional> header
//   2026-08-29 rolled back immediately: restored the in-house Core::Optional<T> (this file)
//
// Design:
//   - stores T + bool has_value_; an empty instance does not construct T (uses union + placement new)
//   - provides value() / operator*() / operator bool() / reset() / emplace()
//   - Core::nullopt is a static empty marker, explicit semantics
//   - closely mirrors the std::optional API, zero cognitive load for callers

#pragma once
#include <stdexcept>
#include <utility>
#include <new>

namespace MyProt { namespace Core {

struct nullopt_t {
    struct tag_t {};
    explicit constexpr nullopt_t(tag_t) {}
};
constexpr nullopt_t nullopt(nullopt_t::tag_t{});

class bad_optional_access : public std::runtime_error {
public:
    bad_optional_access() : std::runtime_error("Core::Optional: bad optional access") {}
};

template <typename T>
class Optional {
public:
    Optional() noexcept : has_value_(false), storage_() {}

    Optional(nullopt_t) noexcept : has_value_(false), storage_() {}

    Optional(const T& v) : has_value_(true), storage_() {
        new (&storage_.value_) T(v);
    }
    Optional(T&& v) : has_value_(true), storage_() {
        new (&storage_.value_) T(std::move(v));
    }

    Optional(const Optional& other) : has_value_(other.has_value_), storage_() {
        if (has_value_) new (&storage_.value_) T(other.storage_.value_);
    }
    Optional(Optional&& other) noexcept : has_value_(other.has_value_), storage_() {
        if (has_value_) {
            new (&storage_.value_) T(std::move(other.storage_.value_));
            other.reset();
        }
    }

    ~Optional() { reset(); }

    Optional& operator=(nullopt_t) noexcept {
        reset();
        return *this;
    }
    Optional& operator=(const T& v) {
        if (has_value_) {
            storage_.value_ = v;
        } else {
            new (&storage_.value_) T(v);
            has_value_ = true;
        }
        return *this;
    }
    Optional& operator=(T&& v) {
        if (has_value_) {
            storage_.value_ = std::move(v);
        } else {
            new (&storage_.value_) T(std::move(v));
            has_value_ = true;
        }
        return *this;
    }
    Optional& operator=(const Optional& other) {
        if (this == &other) return *this;
        if (other.has_value_) {
            *this = other.storage_.value_;
        } else {
            reset();
        }
        return *this;
    }
    Optional& operator=(Optional&& other) noexcept {
        if (this == &other) return *this;
        if (other.has_value_) {
            *this = std::move(other.storage_.value_);
            other.reset();
        } else {
            reset();
        }
        return *this;
    }

    explicit operator bool() const noexcept { return has_value_; }
    bool has_value() const noexcept { return has_value_; }

    T& value() {
        if (!has_value_) throw bad_optional_access();
        return storage_.value_;
    }
    const T& value() const {
        if (!has_value_) throw bad_optional_access();
        return storage_.value_;
    }
    T& operator*() { return value(); }
    const T& operator*() const { return value(); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

    template <typename U>
    T value_or(U&& default_value) const {
        return has_value_ ? storage_.value_ : T(std::forward<U>(default_value));
    }

    void reset() noexcept {
        if (has_value_) {
            storage_.value_.~T();
            has_value_ = false;
        }
    }
    template <typename... Args>
    void emplace(Args&&... args) {
        reset();
        new (&storage_.value_) T(std::forward<Args>(args)...);
        has_value_ = true;
    }

private:
    // storage union: hosts a T with either trivial or non-trivial construction
    union Storage {
        char dummy_;
        T value_;
        Storage() : dummy_() {}
        ~Storage() {}  // actual destruction is controlled by Optional
    } storage_;
    bool has_value_;
};

// comparison operators (convenient for tests such as EXPECT_EQ)
template <typename T>
bool operator==(const Optional<T>& a, const Optional<T>& b) {
    if (a.has_value() != b.has_value()) return false;
    return a.has_value() ? (*a == *b) : true;
}
template <typename T>
bool operator!=(const Optional<T>& a, const Optional<T>& b) { return !(a == b); }
template <typename T>
bool operator==(const Optional<T>& a, nullopt_t) { return !a.has_value(); }
template <typename T>
bool operator==(nullopt_t, const Optional<T>& a) { return !a.has_value(); }
template <typename T>
bool operator==(const Optional<T>& a, const T& v) { return a.has_value() && (*a == v); }
template <typename T>
bool operator==(const T& v, const Optional<T>& a) { return a == v; }

}} // namespace MyProt::Core
