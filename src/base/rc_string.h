#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "src/base/utils.h"

namespace donner {

namespace details {

struct RcStringStorage {
  std::vector<char> data;
};

}  // namespace details

/**
 * A reference counted string, that is copy-on-write.
 */
class RcString {
public:
  static constexpr size_t npos = std::string_view::npos;

  constexpr RcString() {}
  explicit RcString(std::string_view data) { initializeStorage(data); }
  explicit RcString(std::string data) {
    initializeStorage(std::string_view(data.data(), data.size()));
  }

  template <size_t size>
  explicit RcString(const char (&data)[size]) {
    initializeStorage(validateNullTerminatedString(data));
  }

  RcString(const RcString& other) : storage_(other.storage_), str_(other.str_) {}
  RcString(RcString&& other) : storage_(std::move(other.storage_)), str_(std::move(other.str_)) {
    other.str_ = std::string_view();
  }

  RcString& operator=(const RcString& other) {
    storage_ = other.storage_;
    str_ = other.str_;
    return *this;
  }

  RcString& operator=(RcString&& other) {
    storage_ = std::move(other.storage_);
    str_ = std::move(other.str_);
    other.str_ = std::string_view();
    return *this;
  }

  operator std::string_view() const { return str_; }

  // Comparison operators.
  constexpr auto operator==(const char* other) const { return str_ == other; }
  constexpr auto operator==(std::string_view other) const { return str_ == other; }
  constexpr auto operator!=(const char* other) const { return str_ != other; }
  constexpr auto operator!=(std::string_view other) const { return str_ != other; }

  friend std::ostream& operator<<(std::ostream& os, const RcString& self) {
    return os << self.str_;
  }

  /**
   * @return a pointer to the string data.
   */
  const char* data() const { return str_.data(); }

  /**
   * @return if the string is empty.
   */
  bool empty() const { return str_.empty(); }

  /**
   * @return the length of the string.
   */
  size_t size() const { return str_.size(); }

  /**
   * Returns true if the string equals another all-lowercase string, with a case insensitive
   * comparison.
   *
   * @param other string to compare to, must be lowercase.
   * @return true If the strings are equal (case insensitive).
   */
  bool equalsLowercase(std::string_view other) const {
    if (other.size() != str_.size()) {
      return false;
    }

    const std::string_view self = str_;
    for (size_t i = 0; i < other.size(); ++i) {
      if (std::tolower(self[i]) != other[i]) {
        return false;
      }
    }

    return true;
  }

  /**
   * Returns a substring of the string, returning a reference to the original string's data.
   *
   * To copy the data, call `substr(pos, len).duplicate()`.
   *
   * @param pos The position to start the substring.
   * @param len The length of the substring, or `RcString::npos` to return the whole string.
   */
  RcString substr(size_t pos, size_t len = npos) const {
    return RcString(storage_, str_.substr(pos, len));
  }

  /**
   * Duplicates the string, returning a unique reference to the underlying contents. This can be
   * used to reduce memory usage whe taking a substring.
   *
   * @return an RcString with only one owner.
   */
  RcString duplicate() const { return RcString(str_); }

private:
  RcString(std::shared_ptr<details::RcStringStorage> storage, std::string_view str)
      : storage_(std::move(storage)), str_(str) {}

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
    if (UTILS_PREDICT_FALSE(data.empty())) {
      storage_ = nullptr;
      str_ = std::string_view();
    } else {
      const size_t size = data.size();

      storage_ = std::make_unique<details::RcStringStorage>();
      storage_->data.resize(size + 1);
      std::copy(data.begin(), data.end(), storage_->data.begin());
      storage_->data[size] = '\0';

      str_ = std::string_view(storage_->data.data(), size);
    }
  }

  std::shared_ptr<details::RcStringStorage> storage_;
  std::string_view str_;
};

}  // namespace donner
