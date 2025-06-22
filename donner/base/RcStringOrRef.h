#pragma once
/// @file

#include <variant>

#include "donner/base/RcString.h"

namespace donner {

/**
 * An in-transit type that can hold either an \ref RcString or std::string_view, to enable
 * transferring the RcString reference or also accepting a non-owning std::string_view from API
 * surfaces.
 *
 * This can be used either for function arguments, or as a key value for a std::map-like type.
 *
 * As a function:
 * ```
 * void setString(const RcStringOrRef& param) {
 *   // Addrefs if an RcString was passed in
 *  RcString storedStr = param;
 *  // ...
 * }
 *
 *
 * // Passing either an RcString or std::string_view works.
 * setString("test");
 * setString(RcString("will addref if this is larger than the small string optimization");
 * ```
 *
 * As a map key:
 * ```
 * std::map<RcStringOrRef, int> myMap;
 * myMap[RcString("key")] = 1; // Danger! The key lifetime must remain valid.
 *
 * // Lookups are possible with a `std::string_view` or string literal.
 * auto it = myMap.find("key");
 * const bool found = it != myMap.end();
 * assert(found);
 * ```
 */
class RcStringOrRef {
public:
  /// Sentinel value for the maximum value of size_t, used to indicate when the size is not known.
  static constexpr size_t npos = std::string_view::npos;
  /// String iterator.
  using iterator = std::string_view::iterator;
  /// Const string iterator.
  using const_iterator = std::string_view::const_iterator;

  /// Create an empty string.
  constexpr RcStringOrRef() = default;

  /// Destructor.
  ~RcStringOrRef() = default;

  /**
   * Constructs a new RcStringOrRef containing a non-owning std::string_view.
   *
   * @param value Input string to reference.
   */
  /* implicit */ constexpr RcStringOrRef(std::string_view value) : value_(value) {}

  /**
   * Constructs a new RcStringOrRef containing a transferrable RcString.
   *
   * @param value Input string to own.
   */
  /* implicit */ constexpr RcStringOrRef(const RcString& value) : value_(value) {}

  /**
   * Constructs a new RcStringOrRef object from a C-style string reference and optional length.
   *
   * @param value C-style string.
   * @param len Length of the string, or npos to automatically measure, which requires that \ref
   *   data is null-terminated.
   */
  /* implicit */ constexpr RcStringOrRef(const char* value, size_t len = npos)
      : RcStringOrRef(len == npos ? std::string_view(value) : std::string_view(value, len)) {}

  /// Copy constructor.
  constexpr RcStringOrRef(const RcStringOrRef& other) = default;

  /// Move constructor.
  constexpr RcStringOrRef(RcStringOrRef&& other) noexcept : value_(std::move(other.value_)) {
    if (this != &other) {
      other.value_ = std::string_view();
    }
  }

  /// Copy assignment operator.
  RcStringOrRef& operator=(const RcStringOrRef& other) = default;

  /// Move assignment operator.
  RcStringOrRef& operator=(RcStringOrRef&& other) noexcept {
    value_ = std::move(other.value_);
    if (this != &other) {
      other.value_ = std::string_view();
    }
    return *this;
  }

  /// Assignment operator from a C-style string.
  RcStringOrRef& operator=(const char* value) {
    value_ = std::string_view(value);
    return *this;
  }

  /// Assignment operator from a `std::string_view`.
  RcStringOrRef& operator=(std::string_view value) {
    value_ = value;
    return *this;
  }

  /// Assignment operator from an \ref RcString.
  RcStringOrRef& operator=(const RcString& value) {
    value_ = value;
    return *this;
  }

  /// Cast operator to `std::string_view`.
  operator std::string_view() const {
    return std::visit([](auto&& value) { return std::string_view(value); }, value_);
  }

  /// Cast operator to \ref RcString. If the internal storage is a long RcString, it will increment
  /// the refcount without copying.
  operator RcString() const {
    return std::visit([](auto&& value) { return RcString(value); }, value_);
  }

  /// @name Comparison
  /// @{

  /// Spaceship equality operator to another \ref RcStringOrRef.
  friend auto operator<=>(const RcStringOrRef& lhs, const RcStringOrRef& rhs) {
    return compareStringViews(lhs, rhs);
  }

  /// Spaceship equality operator to a StringLike type such as \c std::string_view or \ref RcString.
  template <StringLike StringT>
  friend auto operator<=>(const RcStringOrRef& lhs, const StringT& rhs) {
    return compareStringViews(lhs, rhs);
  }

  //
  // Reversed comparison operators.
  //

  /// Spaceship equality operator to a StringLike type such as \c std::string_view or \ref RcString.
  template <StringLike StringT>
  friend auto operator<=>(const StringT& lhs, const RcStringOrRef& rhs) {
    return compareStringViews(lhs, rhs);
  }

  //
  // For gtest, also implement operator== in terms of operator<=>.
  //

  /// Equality operator to another \ref RcString.
  friend bool operator==(const RcStringOrRef& lhs, const RcStringOrRef& rhs) {
    return (lhs <=> rhs) == std::strong_ordering::equal;
  }

  /// Equality operator to a StringLike type such as \c std::string_view or \ref RcString.
  template <StringLike StringT>
  friend bool operator==(const RcStringOrRef& lhs, const StringT& rhs) {
    return (lhs <=> rhs) == std::strong_ordering::equal;
  }

  /// Reversed equality operator to a StringLike type such as \c std::string_view or \ref
  /// RcString.
  template <StringLike StringT>
  friend bool operator==(const StringT& lhs, const RcStringOrRef& rhs) {
    return (lhs <=> rhs) == std::strong_ordering::equal;
  }

  /// @}

  /// Ostream output operator.
  friend std::ostream& operator<<(std::ostream& os, const RcStringOrRef& self) {
    return os << std::string_view(self);
  }

  /// @name Concatenation
  /// @{

  /// Concatenation operator with another \ref RcStringOrRef.
  friend std::string operator+(const RcStringOrRef& lhs, const RcStringOrRef& rhs) {
    return concatStringViews(lhs, rhs);
  }

  /// Concatenation operator with a C-style string.
  friend std::string operator+(const RcStringOrRef& lhs, const char* rhs) {
    return concatStringViews(lhs, rhs);
  }

  /// Concatenation operator with a `std::string_view`.
  friend std::string operator+(const RcStringOrRef& lhs, std::string_view rhs) {
    return concatStringViews(lhs, rhs);
  }

  /// Reversed concatenation operator with a C-style string.
  friend std::string operator+(std::string_view lhs, const RcStringOrRef& rhs) {
    return concatStringViews(lhs, rhs);
  }

  /// Reversed concatenation operator with a `std::string_view`.
  friend std::string operator+(const char* lhs, const RcStringOrRef& rhs) {
    return concatStringViews(lhs, rhs);
  }

  /// @}

  /**
   * @return a pointer to the string data.
   */
  const char* data() const {
    return std::visit([](auto&& value) { return value.data(); }, value_);
  }

  /**
   * @return if the string is empty.
   */
  bool empty() const { return size() == 0; }

  /**
   * @return the length of the string.
   */
  size_t size() const {
    return std::visit([](auto&& value) { return value.size(); }, value_);
  }

  /**
   * @return the string as a std::string.
   */
  std::string str() const { return std::string(data(), size()); }

  /**
   * Returns a substring of the string.
   *
   * @param pos The position to start the substring.
   * @param len The length of the substring, or `RcStringOrRef::npos` to return the whole string.
   * @return An RcStringOrRef containing the substring.
   */
  RcStringOrRef substr(size_t pos, size_t len = npos) const {
    return std::visit(
        [pos, len](auto&& value) -> RcStringOrRef { return RcStringOrRef(value.substr(pos, len)); },
        value_);
  }

  // Iterators.
  /// Begin iterator.
  const_iterator begin() const noexcept { return cbegin(); }
  /// End iterator.
  const_iterator end() const noexcept { return cend(); }
  /// Begin iterator.
  const_iterator cbegin() const noexcept { return data(); }
  /// End iterator.
  const_iterator cend() const noexcept { return data() + size(); }

  /**
   * Returns true if the string equals another all-lowercase string, with a case insensitive
   * comparison.
   *
   * Example:
   * ```
   * RcString("EXAMPLe").equalsLowercase("example"); // true
   * ```
   *
   * @param lowercaseOther string to compare to, must be lowercase.
   * @return true If the strings are equal (case insensitive).
   */
  bool equalsLowercase(std::string_view lowercaseOther) const {
    return StringUtils::EqualsLowercase(*this, lowercaseOther);
  }

  /**

   * Returns true if the string equals another string with a case-insensitive comparison.
   *
   * @param other string to compare to.
   * @return true If the strings are equal (case insensitive).
   */
  bool equalsIgnoreCase(std::string_view other) const {
    return StringUtils::Equals<StringComparison::IgnoreCase>(*this, other);
  }

private:
  /// Stores either a non-owning reference or RcString which can be transferred without copying.
  std::variant<std::string_view, RcString> value_;

  /**
   * Since libc++ does not support std::basic_string_view::operator<=> yet, we need to implement it.
   * Provides similar functionality to operator<=> using two std::string_views, so that it can be
   * used to implement flavors for RcString, std::string, and std::string_view.
   *
   * @param lhs Left-hand side of the comparison.
   * @param rhs Right-hand side of the comparison.
   * @return std::strong_ordering representing the comparison result, similar to
   *   std::string_view::compare but returning a std::strong_ordering instead of an int.
   */
  static std::strong_ordering compareStringViews(std::string_view lhs, std::string_view rhs) {
    const size_t lhsSize = lhs.size();
    const size_t rhsSize = rhs.size();
    const size_t sharedSize = std::min(lhsSize, rhsSize);

    const int retval = std::char_traits<char>::compare(
        lhs.data(), rhs.data(), sharedSize);  // NOLINT(bugprone-suspicious-stringview-data-usage)
    if (retval == 0) {
      // The shared region matched, return a result based on which string is longer.
      if (lhsSize == rhsSize) {
        return std::strong_ordering::equal;
      } else if (lhsSize < rhsSize) {
        return std::strong_ordering::less;
      } else {
        return std::strong_ordering::greater;
      }
    } else if (retval < 0) {
      return std::strong_ordering::less;
    } else {
      return std::strong_ordering::greater;
    }
  }

  static std::string concatStringViews(std::string_view lhs, std::string_view rhs) {
    std::string result;
    result.reserve(lhs.size() + rhs.size());
    result.append(lhs);
    result.append(rhs);
    return result;
  }
};

}  // namespace donner

/**
 * Hash function for RcString.
 */
template <>
struct std::hash<donner::RcStringOrRef> {
  /**
   * Hash function for RcStringOrRef.
   *
   * @param str Input string.
   * @return std::size_t Output hash.
   */
  std::size_t operator()(const donner::RcStringOrRef& str) const {
    return std::hash<std::string_view>()(str);
  }
};
