#pragma once
/// @file

#include <bit>
#include <cassert>
#include <compare>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/StringUtils.h"

namespace donner {

/**
 * A reference counted string, that is copy-on-write and implements the small-string
 * optimization.
 *
 * Implements a short-string optimization similar to the libc++ `std::string` class, see
 * https://joellaity.com/2020/01/31/string.html for details.
 *
 * USAGE NOTES:
 * - This class is not thread-safe, and should not be shared between threads without
 * synchronization.
 * - \ref data() is not guaranteed to remain stable across references, and should not be stored
 * longer than the original RcString which created it.
 */
class RcString {
public:
  /*
   * Since we use the low bit to indicate whether the string is a short or long string, aliasing the
   * one-byte size in the first field of ShortStringData with size_t size of LongStringData.
   *
   * On big-endian architectures, we would need to use the high bit instead. Since I don't have a
   * big-endian machine to test, only little-endian is currently supported.
   */
  static_assert(std::endian::native == std::endian::little, "Only little-endian is supported");

  /// Sentinel value for the maximum value of size_t, used to indicate when the size is not known.
  static constexpr size_t npos = std::string_view::npos;
  /// String iterator.
  using iterator = std::string_view::iterator;
  /// Const string iterator.
  using const_iterator = std::string_view::const_iterator;

  /// Create an empty string.
  RcString() = default;

  /// Destructor.
  ~RcString() = default;

  /**
   * Constructs a new RcString object by copying an existing string.
   *
   * @param data Input string to copy.
   */
  explicit RcString(std::string_view data) { initializeStorage(data); }

  /**
   * Constructs a new RcString object from a C-style string and optional length.
   *
   * @param data C-style string.
   * @param len Length of the string, or npos to automatically measure, which requires that \ref
   *   data is null-terminated.
   */
  /* implicit */ RcString(const char* data, size_t len = npos)
      : RcString(len == npos ? std::string_view(data) : std::string_view(data, len)) {}

  /**
   * Constructs an RcString by consuming an existing vector.
   *
   * @param data Input data to consume, moved into the RcString storage.
   * @return RcString using the input data.
   */
  static RcString fromVector(std::vector<char>&& data) { return RcString(std::move(data)); }

  /// Copy constructor.
  RcString(const RcString& other) = default;
  /// Move constructor.
  RcString(RcString&& other) noexcept = default;

  /// Copy assignment operator.
  RcString& operator=(const RcString& other) = default;

  /// Move assignment operator.
  RcString& operator=(RcString&& other) noexcept = default;

  /// Assignment operator from a C-style string.
  RcString& operator=(const char* data) {
    initializeStorage(std::string_view(data));
    return *this;
  }

  /// Assignment operator from a `std::string_view`.
  RcString& operator=(std::string_view data) {
    initializeStorage(data);
    return *this;
  }

  /// Cast operator to `std::string_view`.
  operator std::string_view() const {
    return data_.isLong() ? data_.long_.view() : data_.short_.view();
  }

  /// @name Comparison
  /// @{

  /// Spaceship equality operator to another \ref RcString.
  friend auto operator<=>(const RcString& lhs, const RcString& rhs) {
    return compareStringViews(lhs, rhs);
  }

  /// Spaceship equality operator to a C-style string.
  friend auto operator<=>(const RcString& lhs, const char* rhs) {
    return compareStringViews(lhs, rhs);
  }

  /// Spaceship equality operator to a `std::string_view`.
  friend auto operator<=>(const RcString& lhs, std::string_view rhs) {
    return compareStringViews(lhs, rhs);
  }

  //
  // Reversed comparison operators.
  //

  /// Spaceship equality operator to another \ref RcString.
  friend auto operator<=>(const char* lhs, const RcString& rhs) {
    return compareStringViews(lhs, rhs);
  }

  /// Spaceship equality operator to a C-style string.
  friend auto operator<=>(std::string_view lhs, const RcString& rhs) {
    return compareStringViews(lhs, rhs);
  }

  //
  // For gtest, also implement operator== in terms of operator<=>.
  //

  /// Equality operator to another \ref RcString.
  friend bool operator==(const RcString& lhs, const RcString& rhs) {
    return (lhs <=> rhs) == std::strong_ordering::equal;
  }

  /// Equality operator to a C-style string.
  friend bool operator==(const RcString& lhs, const char* rhs) {
    return (lhs <=> rhs) == std::strong_ordering::equal;
  }

  /// Equality operator to a `std::string_view`.
  friend bool operator==(const RcString& lhs, std::string_view rhs) {
    return (lhs <=> rhs) == std::strong_ordering::equal;
  }

  /// Reversed equality operator to another \ref RcString.
  friend bool operator==(const char* lhs, const RcString& rhs) {
    return (lhs <=> rhs) == std::strong_ordering::equal;
  }

  /// Reversed equality operator to a C-style string.
  friend bool operator==(std::string_view lhs, const RcString& rhs) {
    return (lhs <=> rhs) == std::strong_ordering::equal;
  }

  /// @}

  /// Ostream output operator.
  friend std::ostream& operator<<(std::ostream& os, const RcString& self) {
    return os << std::string_view(self);
  }

  /// @name Concatenation
  /// @{

  /// Concatenation operator with another \ref RcString.
  friend std::string operator+(const RcString& lhs, const RcString& rhs) {
    return concatStringViews(lhs, rhs);
  }

  /// Concatenation operator with a C-style string.
  friend std::string operator+(const RcString& lhs, const char* rhs) {
    return concatStringViews(lhs, rhs);
  }

  /// Concatenation operator with a `std::string_view`.
  friend std::string operator+(const RcString& lhs, std::string_view rhs) {
    return concatStringViews(lhs, rhs);
  }

  /// Reversed concatenation operator with a C-style string.
  friend std::string operator+(std::string_view lhs, const RcString& rhs) {
    return concatStringViews(lhs, rhs);
  }

  /// Reversed concatenation operator with a `std::string_view`.
  friend std::string operator+(const char* lhs, const RcString& rhs) {
    return concatStringViews(lhs, rhs);
  }

  /// @}

  /**
   * @return a pointer to the string data.
   */
  const char* data() const { return data_.isLong() ? data_.long_.data : &data_.short_.data[0]; }

  /**
   * @return if the string is empty.
   */
  bool empty() const { return size() == 0; }

  /**
   * @return the length of the string.
   */
  size_t size() const { return data_.isLong() ? data_.long_.size() : data_.short_.size(); }

  /**
   * @return the string as a std::string.
   */
  std::string str() const { return std::string(data(), size()); }

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

  /**
   * Returns a substring of the string, returning a reference to the original string's data. Note
   * that due to the short-string optimization, this may not always reference the original data
   * and may contain a copy if the string is below the short-string threshold.
   *
   * @param pos The position to start the substring.
   * @param len The length of the substring, or `RcString::npos` to return the whole string.
   */
  RcString substr(size_t pos, size_t len = npos) const {
    const std::string_view slice = std::string_view(*this).substr(pos, len);

    if (data_.isLong() && slice.size() > kShortStringCapacity) {
      return RcString(data_.long_.storage, slice);
    } else {
      return RcString(slice);
    }
  }

  /**
   * Deduplicates the string, updating its underlying storage to ensure that it has a unique
   * reference to the underlying contents. This can be used to reduce memory usage whe taking a
   * substring.
   *
   * Has no effect if the string is short due to the short-string optimization.
   */
  void dedup() {
    if (data_.isLong()) {
      *this = RcString(std::string_view(*this));
    }
  }

private:
  // One bit is reserved.
  static constexpr size_t kMaxSize = size_t(1) << ((sizeof(size_t) * 8) - 1);

  struct LongStringData {
    struct InitOnlySharedPtr {};

    // Only initialize the storage field, leave everything else unset.
    // NOLINTNEXTLINE: Only one field is initialized as an optimization.
    constexpr LongStringData(InitOnlySharedPtr) : storage(nullptr) {}

    LongStringData(std::shared_ptr<std::vector<char>> storageRef, std::string_view view)
        : shiftedSize((view.size() << 1) | 1), data(view.data()), storage(std::move(storageRef)) {}

    ~LongStringData() { storage = nullptr; }

    // Default move and copy constructors.
    LongStringData(const LongStringData&) = default;
    LongStringData& operator=(const LongStringData&) = default;
    LongStringData(LongStringData&&) = default;
    LongStringData& operator=(LongStringData&&) = default;

    size_t shiftedSize;
    const char* data;
    std::shared_ptr<std::vector<char>> storage;

    size_t size() const { return shiftedSize >> 1; }
    std::string_view view() const { return std::string_view(data, size()); }
  };

  static constexpr size_t kShortStringCapacity = sizeof(LongStringData) - 1;
  static_assert(kShortStringCapacity < 128,
                "Short string capacity must leave one bit for long string flag.");

  struct ShortStringData {
    constexpr ShortStringData() {}  // NOLINT(cppcoreguidelines-pro-type-member-init): data is not
                                    // initialized by default due to use in union

    uint8_t shiftedSizeByte = 0;
    char data[kShortStringCapacity];

    size_t size() const { return shiftedSizeByte >> 1; }
    std::string_view view() const { return std::string_view(&data[0], size()); }
  };

  static_assert(sizeof(LongStringData) == sizeof(ShortStringData),
                "Long and short string data must be the same size.");

  explicit RcString(std::shared_ptr<std::vector<char>> storage, std::string_view view)
      : data_(std::move(storage), view) {}

  /**
   * Construct an RcString from an existing vector.
   *
   * @param data Vector to move into the RcString.
   */
  explicit RcString(std::vector<char>&& data) {
    assert(data.size() <= kMaxSize);

    data_.long_.shiftedSize = (data.size() << 1) | 1;
    data_.long_.data = data.data();
    data_.long_.storage = std::make_shared<std::vector<char>>(std::move(data));
  }

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
  constexpr static std::strong_ordering compareStringViews(std::string_view lhs,
                                                           std::string_view rhs) {
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

  void initializeStorage(std::string_view data) {
    const size_t size = data.size();

    if (size <= kShortStringCapacity) {
      data_.short_.shiftedSizeByte = static_cast<uint8_t>(size) << 1;
      std::copy(data.begin(), data.end(), &data_.short_.data[0]);
    } else {
      assert(size < kMaxSize);

      data_.long_.shiftedSize = (size << 1) | 1;
      data_.long_.storage = std::make_shared<std::vector<char>>(size);

      std::vector<char>& vec = *data_.long_.storage.get();
      std::copy(data.begin(), data.end(), vec.begin());
      data_.long_.data = vec.data();
    }
  }

  union Storage {
    LongStringData long_;
    ShortStringData short_;

    Storage() : short_() {
      // Call the empty LongStringData constructor, to clear the field containing the shared_ptr so
      // we don't need to zero the entire short_ buffer.
      new (&long_) LongStringData(LongStringData::InitOnlySharedPtr());
    }
    explicit Storage(std::shared_ptr<std::vector<char>> storage, std::string_view view)
        : long_(std::move(storage), view) {}

    ~Storage() { clear(); }

    Storage(const Storage& other) {
      if (other.isLong()) {
        // Specifically use the placement new operator since long_ has not been initialized yet.
        new (&long_) LongStringData(other.long_);
      } else {
        short_ = other.short_;
      }
    }

    Storage(Storage&& other) noexcept {
      if (other.isLong()) {
        // Specifically use the placement new operator since long_ has not been initialized yet.
        new (&long_) LongStringData(std::move(other.long_));
        other.long_.storage = nullptr;

        // Set length to zero, which also switches this to a short string.
        other.short_.shiftedSizeByte = 0;
      } else {
        short_ = other.short_;
        other.short_.shiftedSizeByte = 0;
      }
    }

    Storage& operator=(const Storage& other) {
      if (this != &other) {
        clear();

        if (other.isLong()) {
          long_ = other.long_;
        } else {
          short_ = other.short_;
        }
      }

      return *this;
    }

    Storage& operator=(Storage&& other) noexcept {
      if (this != &other) {
        clear();

        if (other.isLong()) {
          long_ = std::move(other.long_);
          other.long_.storage = nullptr;

          // Set length to zero, which also switches this to a short string.
          other.short_.shiftedSizeByte = 0;
        } else {
          short_ = other.short_;
          other.short_.shiftedSizeByte = 0;
        }
      }

      return *this;
    }

    void clear() {
      if (isLong()) {
        long_.~LongStringData();
      }

      // Initialize empty long string, to clear the shared_ptr.
      new (&long_) LongStringData(LongStringData::InitOnlySharedPtr());
    }

    bool isLong() const { return (short_.shiftedSizeByte & 1) == 1; }
  };

  Storage data_;
};

}  // namespace donner

/**
 * Hash function for RcString.
 */
template <>
struct std::hash<donner::RcString> {
  /**
   * Hash function for RcString.
   *
   * @param str Input string.
   * @return std::size_t Output hash.
   */
  std::size_t operator()(const donner::RcString& str) const {
    return std::hash<std::string_view>()(str);
  }
};
