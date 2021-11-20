#pragma once

#include <bit>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace donner {

/**
 * A reference counted string, that is copy-on-write.
 *
 * Implements a short-string optimization similar to the libc++ std::string class, see
 * https://joellaity.com/2020/01/31/string.html for details.
 */
class RcString {
public:
  /**
   * Since we use the low bit to indicate whether the string is a short or long string, aliasing the
   * one-byte size in the first field of ShortStringData with size_t size of LongStringData.
   *
   * On big-endian architectures, we would need to use the high bit instead. Since I don't have a
   * big-endian machine to test, only little-endian is currently supported.
   */
  static_assert(std::endian::native == std::endian::little, "Only little-endian is supported");

  static constexpr size_t npos = std::string_view::npos;

  constexpr RcString() = default;
  explicit RcString(std::string_view data) { initializeStorage(data); }
  explicit RcString(const std::string& data) {
    initializeStorage(std::string_view(data.data(), data.size()));
  }

  template <size_t size>
  explicit RcString(const char (&data)[size]) {
    initializeStorage(validateNullTerminatedString(data));
  }

  RcString(const RcString& other) : data_(other.data_) {}
  RcString(RcString&& other) : data_(std::move(other.data_)) {}

  RcString& operator=(const RcString& other) {
    data_ = other.data_;
    return *this;
  }

  RcString& operator=(RcString&& other) {
    data_ = std::move(other.data_);
    return *this;
  }

  operator std::string_view() const {
    return data_.isLong() ? std::string_view(data_.long_.data, data_.long_.size())
                          : std::string_view(data_.short_.data, data_.short_.size());
  }

  // Comparison operators.
  constexpr auto operator==(const char* other) const { return std::string_view(*this) == other; }
  constexpr auto operator==(std::string_view other) const {
    return std::string_view(*this) == other;
  }
  constexpr auto operator!=(const char* other) const { return std::string_view(*this) != other; }
  constexpr auto operator!=(std::string_view other) const {
    return std::string_view(*this) != other;
  }

  friend std::ostream& operator<<(std::ostream& os, const RcString& self) {
    return os << std::string_view(self);
  }

  /**
   * @return a pointer to the string data.
   */
  const char* data() const { return data_.isLong() ? data_.long_.data : data_.short_.data; }

  /**
   * @return if the string is empty.
   */
  bool empty() const { return data_.short_.size() == 0; }

  /**
   * @return the length of the string.
   */
  size_t size() const { return data_.isLong() ? data_.long_.size() : data_.short_.size(); }

  /**
   * Returns true if the string equals another all-lowercase string, with a case insensitive
   * comparison.
   *
   * @param other string to compare to, must be lowercase.
   * @return true If the strings are equal (case insensitive).
   */
  bool equalsLowercase(std::string_view other) const {
    if (other.size() != size()) {
      return false;
    }

    const std::string_view self = std::string_view(*this);
    for (size_t i = 0; i < other.size(); ++i) {
      if (std::tolower(self[i]) != other[i]) {
        return false;
      }
    }

    return true;
  }

  /**
   * Returns a substring of the string, returning a reference to the original string's data. Note
   * that due to the short-string optimization, this may not always reference the original data and
   * may contain a copy if the string is below the short-string threshold.
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
  struct LongStringData {
    LongStringData() : storage(nullptr) {}
    LongStringData(std::shared_ptr<std::vector<char>> storage, std::string_view view)
        : shiftedSize((view.size() << 1) | 1), data(view.data()), storage(std::move(storage)) {}

    ~LongStringData() { storage = nullptr; }

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
    constexpr ShortStringData() : shiftedSizeByte(0) {}

    uint8_t shiftedSizeByte;
    char data[kShortStringCapacity];

    size_t size() const { return shiftedSizeByte >> 1; }
    std::string_view view() const { return std::string_view(data, size()); }
  };

  static_assert(sizeof(LongStringData) == sizeof(ShortStringData),
                "Long and short string data must be the same size.");

  explicit RcString(std::shared_ptr<std::vector<char>> storage, std::string_view view)
      : data_(std::move(storage), view) {}

  /**
   * Validates if a given string literal buffer is null-terminated, and then returns a string_view
   * to its contents.
   *
   * @param data Buffer array.
   * @param size Buffer size, including the null terminator.
   * @return std::string_view to the string contents, or an assert.
   */
  template <size_t size>
  static std::string_view validateNullTerminatedString(const char (&data)[size]) {
    assert(size > 0 && data[size - 1] == '\0');
    return std::string_view(data, size - 1);
  }

  void initializeStorage(std::string_view data) {
    const size_t size = data.size();

    if (size <= kShortStringCapacity) {
      data_.short_.shiftedSizeByte = static_cast<uint8_t>(size) << 1;
      std::copy(data.begin(), data.end(), &data_.short_.data[0]);
    } else {
      // One bit is reserved.
      constexpr size_t kMaxSize = size_t(1) << ((sizeof(size_t) * 8) - 1);
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

    constexpr Storage() : short_() {
      // Call the empty LongStringData constructor, to clear the field containing the shared_ptr so
      // we don't need to zero the entrire short_ buffer.
      new (&long_) LongStringData();
    }
    explicit Storage(std::shared_ptr<std::vector<char>> storage, std::string_view view)
        : long_(std::move(storage), view) {}

    ~Storage() { clear(); }

    Storage(const Storage& other) { *this = other; }
    Storage(Storage&& other) { *this = std::move(other); }

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

    Storage& operator=(Storage&& other) {
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
      new (&long_) LongStringData();
    }

    bool isLong() const { return (short_.shiftedSizeByte & 1) == 1; }
  };

  Storage data_;
};

}  // namespace donner
