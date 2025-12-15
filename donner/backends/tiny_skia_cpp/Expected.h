#pragma once
/// @file

#include <cassert>
#include <utility>
#include <variant>

namespace donner::backends::tiny_skia_cpp {

/**
 * Lightweight alternative to std::expected until C++23 is available.
 *
 * Stores either a value or an error without relying on exceptions. Accessing the inactive
 * alternative triggers an assertion in debug builds.
 */
template <typename T, typename E>
class Expected {
public:
  /// Creates a success value by copying \a value.
  static Expected Success(const T& value) { return Expected(std::in_place_index<1>, value); }

  /// Creates a success value by moving \a value.
  static Expected Success(T&& value) { return Expected(std::in_place_index<1>, std::move(value)); }

  /// Creates a failure value by copying \a error.
  static Expected Failure(const E& error) { return Expected(std::in_place_index<2>, error); }

  /// Creates a failure value by moving \a error.
  static Expected Failure(E&& error) { return Expected(std::in_place_index<2>, std::move(error)); }

  /// Returns true when the instance holds a value.
  bool hasValue() const { return hasValue_; }

  /// Allows truthiness checks: `if (result) { ... }`.
  explicit operator bool() const { return hasValue_; }

  /// Returns a reference to the stored value. Asserts when no value is present.
  const T& value() const {
    assert(hasValue_);
    return std::get<1>(storage_);
  }

  /// Returns a mutable reference to the stored value. Asserts when no value is present.
  T& value() {
    assert(hasValue_);
    return std::get<1>(storage_);
  }

  /// Returns a reference to the stored error. Asserts when a value is present.
  const E& error() const {
    assert(!hasValue_);
    return std::get<2>(storage_);
  }

  /// Returns a mutable reference to the stored error. Asserts when a value is present.
  E& error() {
    assert(!hasValue_);
    return std::get<2>(storage_);
  }

  /// Returns the value when present, otherwise returns \a fallback.
  template <typename U>
  T valueOr(U&& fallback) const {
    if (hasValue_) {
      return std::get<1>(storage_);
    }
    return static_cast<T>(std::forward<U>(fallback));
  }

private:
  template <typename... Args>
  Expected(std::in_place_index_t<1> inPlace, Args&&... args)
      : storage_(inPlace, std::forward<Args>(args)...), hasValue_(true) {}

  template <typename... Args>
  Expected(std::in_place_index_t<2> inPlace, Args&&... args)
      : storage_(inPlace, std::forward<Args>(args)...), hasValue_(false) {}

  std::variant<std::monostate, T, E> storage_;
  bool hasValue_ = false;
};

}  // namespace donner::backends::tiny_skia_cpp
