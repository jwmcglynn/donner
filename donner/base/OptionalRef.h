#pragma once
/// @file

#include <cassert>
#include <optional>
#include <ostream>

namespace donner {

/**
 * A class that simulates an optional reference to a constant object of type `T`.
 *
 * This class behaves similarly to `std::optional<const T&>`, which is proposed by the upcoming
 * [P2988 std::optional<T&>](https://github.com/cplusplus/papers/issues/1661) proposal, providing a
 * way to store  an optional reference to a constant object. Since `std::optional` cannot be used
 * with reference types, `OptionalRef` provides a workaround.
 *
 * @note The referenced object must outlive the `OptionalRef`. It is the responsibility of the user
 * to ensure that the reference remains valid.
 *
 * @tparam T The type of the object to reference.
 */
template <typename T>
class OptionalRef {
public:
  /**
   * Constructs an empty `OptionalRef`.
   */
  constexpr OptionalRef() noexcept = default;

  /**
   * Constructs an empty `OptionalRef` from a `std::nullopt`.
   *
   * @param nullopt Accepts `std::nullopt` to construct an empty `OptionalRef`.
   */
  /* implicit */ OptionalRef(std::nullopt_t nullopt) noexcept {}

  /**
   * Constructs an `OptionalRef` that contains a reference to the given object.
   *
   * @param ref The object to reference.
   */
  /* implicit */ constexpr OptionalRef(const T& ref) noexcept : ptr_(&ref) {}

  /// Destructor.
  ~OptionalRef() noexcept = default;

  /**
   * Copy constructor.
   *
   * @param other The other `OptionalRef` to copy from.
   */
  constexpr OptionalRef(const OptionalRef& other) noexcept = default;

  /**
   * Move constructor.
   *
   * @param other The other `OptionalRef` to move from.
   */
  constexpr OptionalRef(OptionalRef&& other) noexcept = default;

  /**
   * Copy assignment operator.
   *
   * @param other The other `OptionalRef` to copy from.
   * @return Reference to this `OptionalRef`.
   */
  OptionalRef& operator=(const OptionalRef& other) noexcept = default;

  /**
   * Move assignment operator.
   *
   * @param other The other `OptionalRef` to move from.
   * @return Reference to this `OptionalRef`.
   */
  OptionalRef& operator=(OptionalRef&& other) noexcept = default;

  /**
   * Assigns a reference to the given object.
   *
   * @param ref The object to reference.
   * @return Reference to this `OptionalRef`.
   */
  OptionalRef& operator=(const T& ref) noexcept {
    ptr_ = &ref;
    return *this;
  }

  /**
   * Resets the `OptionalRef` to be empty.
   */
  void reset() noexcept { ptr_ = nullptr; }

  /**
   * Returns `true` if the `OptionalRef` contains a reference.
   *
   * @return `true` if the `OptionalRef` is not empty.
   */
  constexpr explicit operator bool() const noexcept { return ptr_ != nullptr; }

  /**
   * Returns `true` if the `OptionalRef` contains a reference..
   *
   * @return `true` if the `OptionalRef` is not empty.
   */
  constexpr bool hasValue() const noexcept { return ptr_ != nullptr; }

  /**
   * Returns a const reference to the referenced object.
   *
   * @return Const reference to the object.
   *
   * @note Asserts that the `OptionalRef` is not empty.
   */
  constexpr const T& value() const {
    assert(ptr_ && "OptionalRef::value() called on empty OptionalRef");
    return *ptr_;
  }

  /**
   * Returns a const reference to the referenced object.
   *
   * @return Const reference to the object.
   *
   * @note Asserts that the `OptionalRef` is not empty.
   */
  constexpr const T& operator*() const { return value(); }

  /**
   * Returns a pointer to the referenced object.
   *
   * @return Pointer to the object.
   *
   * @note Asserts that the `OptionalRef` is not empty.
   */
  constexpr const T* operator->() const noexcept {
    assert(ptr_ && "OptionalRef::operator->() called on empty OptionalRef");
    return ptr_;
  }

  /// Equality operator to another `OptionalRef`.
  constexpr friend bool operator==(const OptionalRef& lhs, const OptionalRef& rhs) {
    return lhs.hasValue() == rhs.hasValue() && (!lhs.hasValue() || lhs.value() == rhs.value());
  }

  /// Equality operator to a `std::nullopt`.
  constexpr friend bool operator==(const OptionalRef& lhs, std::nullopt_t) { return !lhs; }

  /// Equality operator for a set value.
  constexpr friend bool operator==(const OptionalRef& lhs, const T& rhs) {
    return lhs.hasValue() && lhs.value() == rhs;
  }

  /**
   * Stream output operator.
   *
   * @param os Output stream.
   * @param opt The `OptionalRef` to output.
   * @return Reference to the output stream.
   *
   * @note Outputs the referenced object if not empty, otherwise outputs `"nullopt"`.
   */
  friend std::ostream& operator<<(std::ostream& os, const OptionalRef& opt) {
    if (opt) {
      return os << *opt;
    } else {
      return os << "nullopt";
    }
  }

private:
  const T* ptr_ = nullptr;  //!< Pointer to the referenced object, or `nullptr` if empty.
};

}  // namespace donner
