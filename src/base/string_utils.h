#pragma once

#include <concepts>
#include <iterator>
#include <string>
#include <string_view>

// TODO(toolchain): Switch to <ranges> once libc++ supports std::views::split and
// std::views::transform.
#include <range/v3/view/split.hpp>
#include <range/v3/view/transform.hpp>

namespace donner {

template <typename T>
concept StringLike = requires(T t, size_t i) {
  { t.size() } -> std::same_as<size_t>;
  { t.data() } -> std::same_as<const char*>;
};

enum class StringComparison { Default, IgnoreCase };

struct CaseInsensitiveCharTraits : public std::char_traits<char> {
  static bool eq(char lhs, char rhs) { return std::tolower(lhs) == std::tolower(rhs); }
  static bool ne(char lhs, char rhs) { return std::tolower(lhs) != std::tolower(rhs); }
  static bool lt(char lhs, char rhs) { return std::tolower(lhs) < std::tolower(rhs); }

  static int compare(const char* lhs, const char* rhs, size_t sizeToCompare) {
    for (size_t i = 0; i < sizeToCompare; ++i) {
      const char lhsCh = std::tolower(lhs[i]);
      const char rhsCh = std::tolower(rhs[i]);
      if (lhsCh < rhsCh) {
        return -1;
      } else if (lhsCh > rhsCh) {
        return 1;
      }
    }

    return 0;
  }

  static const char* find(const char* str, size_t size, char ch) {
    const char lowerCh = std::tolower(ch);
    for (size_t i = 0; i < size; ++i) {
      if (std::tolower(str[i]) == lowerCh) {
        return &str[i];
      }
    }

    return nullptr;
  }
};

/**
 * A collection of string utils, such as case-insensitive comparison and begins/endsWith.
 */
class StringUtils {
public:
  /**
   * Compare two strings. Returns true if the string equals the other all-lowercase string, with a
   * case insensitive comparison.
   *
   * @param lhs The first string to compare, can be any case.
   * @param lowercaseRhs string to compare to, must be lowercase.
   * @return true If the \ref lowercaseRhs is equal to the \ref lhs, ignoring the case of the \ref
   * lhs.
   */
  template <StringLike T, StringLike U>
  static bool EqualsLowercase(const T& lhs, const U& lowercaseRhs) {
    if (lhs.size() != lowercaseRhs.size()) {
      return false;
    }

    const char* lhsData = lhs.data();
    const char* rhsData = lowercaseRhs.data();
    for (size_t i = 0; i < lowercaseRhs.size(); ++i) {
      if (std::tolower(lhsData[i]) != rhsData[i]) {
        return false;
      }
    }

    return true;
  }

  /**
   * Returns true if two string are equal with a case-insensitive comparison.
   *
   * @param lhs The first string to compare.
   * @param rhs The second string to compare.
   * @tparam Comparison The comparison type to use, defaults to \ref StringComparison::Default.
   * @return true If the strings are equal.
   */
  template <StringComparison Comparison = StringComparison::Default, StringLike T, StringLike U>
  static bool Equals(const T& lhs, const U& rhs) {
    if (lhs.size() != rhs.size()) {
      return false;
    }

    return CharArraysEqual<Comparison>(lhs.data(), rhs.data(), lhs.size());
  }

  /**
   * Returns true if \ref str starts with \ref otherStr.
   *
   * @param str The string to check for a prefix.
   * @param otherStr The prefix to check for.
   * @tparam Comparison The comparison type to use, defaults to \ref StringComparison::Default.
   * @return true If the strings are equal.
   */
  template <StringComparison Comparison = StringComparison::Default, StringLike T, StringLike U>
  static bool StartsWith(const T& str, const U& otherStr) {
    if (str.size() < otherStr.size()) {
      return false;
    }

    return CharArraysEqual<Comparison>(str.data(), otherStr.data(), otherStr.size());
  }

  /**
   * Returns true if \ref str ends with \ref otherStr.
   *
   * @param str The string to check for a suffix.
   * @param otherStr The suffix to check for.
   * @tparam Comparison The comparison type to use, defaults to \ref StringComparison::Default.
   * @return true If the strings are equal.
   */
  template <StringComparison Comparison = StringComparison::Default, StringLike T, StringLike U>
  static bool EndsWith(const T& str, const U& otherStr) {
    if (str.size() < otherStr.size()) {
      return false;
    }

    const char* data = str.data() + str.size() - otherStr.size();
    const char* otherData = otherStr.data();
    return CharArraysEqual<Comparison>(data, otherData, otherStr.size());
  }

  /**
   * Returns true if \ref str contains \ref otherStr.
   *
   * @param str The string to check for a suffix.
   * @param otherStr The suffix to check for.
   * @tparam Comparison The comparison type to use, defaults to \ref StringComparison::Default.
   * @return true If the strings are equal.
   */
  template <StringComparison Comparison = StringComparison::Default, StringLike T, StringLike U>
  static bool Contains(const T& str, const U& otherStr) {
    if (str.size() < otherStr.size()) {
      return false;
    }

    using StringViewType = std::basic_string_view<
        char, std::conditional_t<Comparison == StringComparison::Default, std::char_traits<char>,
                                 CaseInsensitiveCharTraits>>;

    const StringViewType strView(str.data(), str.size());
    const StringViewType otherStrView(otherStr.data(), otherStr.size());

    return strView.find(otherStrView) != StringViewType::npos;
  }

  /**
   * Splits a string by a given character, returning a range of the split strings as a \ref
   * std::string_view.
   *
   * Example:
   * @code{.cpp}
   * for (auto&& str : Split("a,b,c", ',')) {
   *   // ...
   * }
   * @endcode
   *
   * @tparam T The string type to split.
   * @param str The string to split.
   * @param ch The character to split by.
   * @return auto Range containing split string views.
   */
  template <StringLike T>
  static auto Split(const T& str, char ch = ' ') {
    const std::string_view strView(str.data(), str.size());
    return strView | ranges::views::split(ch) | ranges::views::transform([](auto&& rng) {
             return std::string_view(&*rng.begin(), ranges::distance(rng));
           });
  }

private:
  /**
   * Compares two character arrays, using a StringComparison type to determine if the comparison is
   * case-sensitive.
   *
   * @tparam Comparison String comparison type.
   * @param lhs First string to compare.
   * @param rhs Second string to compare.
   * @param sizeToCompare The number of characters to compare, \ref lhs and \ref rhs must be at
   * least this size.
   * @return true If the strings are equal.
   */
  template <StringComparison Comparison>
  static bool CharArraysEqual(const char* lhs, const char* rhs, size_t sizeToCompare) {
    if constexpr (Comparison == StringComparison::IgnoreCase) {
      return CaseInsensitiveCharTraits::compare(lhs, rhs, sizeToCompare) == 0;
    } else {
      return std::char_traits<char>::compare(lhs, rhs, sizeToCompare) == 0;
    }
  }
};

}  // namespace donner
