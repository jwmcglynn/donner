#pragma once
/// @file

#include <concepts>
#include <iterator>
#include <string>
#include <string_view>

// TODO(toolchain): Switch to <ranges> once libc++ supports std::views::split and
// std::views::transform.
#include <range/v3/range/conversion.hpp>
#include <range/v3/view/filter.hpp>
#include <range/v3/view/split.hpp>
#include <range/v3/view/transform.hpp>

namespace donner {

namespace details {

/**
 * A concept for types that are C-style strings, i.e. `const char*` or `char*`.
 *
 * @tparam T C-style string type.
 */
template <typename T>
concept IsCStr = std::is_same_v<T, const char*> || std::is_same_v<T, char*>;

}  // namespace details

/**
 * A concept for types that are string-like, i.e. have a `size()` and `data()` method.
 *
 * @tparam T `std::string`-like type.
 */
template <typename T>
concept StringLike = requires(T t, size_t i) {
                       { t.size() } -> std::same_as<size_t>;
                       { t.data() } -> details::IsCStr;
                     };

/// String comparison options, e.g. case sensitivity.
enum class StringComparison {
  Default,     ///< The default case-sensitive string comparison.
  IgnoreCase,  ///< Case-insensitive string comparison.
};

/**
 * Type traits for case-insensitive string comparison, usable with algorithms that accept an STL
 * `std::char_traits`.
 */
struct CaseInsensitiveCharTraits : public std::char_traits<char> {
  /**
   * Compare two characters for equality.
   *
   * @param lhs Left-hand side character.
   * @param rhs Right-hand side character.
   */
  static bool eq(char lhs, char rhs) { return std::tolower(lhs) == std::tolower(rhs); }

  /**
   * Compare two characters for inequality.
   *
   * @param lhs Left-hand side character.
   * @param rhs Right-hand side character.
   */
  static bool ne(char lhs, char rhs) { return std::tolower(lhs) != std::tolower(rhs); }

  /**
   * Compare two characters for less-than.
   *
   * @param lhs Left-hand side character.
   * @param rhs Right-hand side character.
   */
  static bool lt(char lhs, char rhs) { return std::tolower(lhs) < std::tolower(rhs); }

  /**
   * Compare two strings for equality, returning -1 if \p lhs is less than \p rhs, 0 if they are
   * equal, or 1 if \p lhs is greater than \p rhs.
   *
   * @param lhs Left-hand side string.
   * @param rhs Right-hand side string.
   * @param sizeToCompare The number of characters to compare.
   */
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

  /**
   * Find the first occurrence of a character in a string using a case-insensitive match.
   *
   * @param str The string to search.
   * @param size The size of the string.
   * @param ch The character to search for.
   */
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
 * A collection of string utils, such as case-insensitive comparison and StartsWith/EndsWith.
 */
class StringUtils {
public:
  /**
   * Compare two strings with case-insensitive comparison, fast-path assuming that one of the
   * strings is all-lowercase.
   *
   * ```
   * StringUtils::EqualsLowercase("Hello", "hello"); // true
   * StringUtils::EqualsLowercase("Hello", "HELLO"); // invalid, rhs must be lowercase
   * ```
   *
   * @param lhs The first string to compare, can be any case.
   * @param lowercaseRhs string to compare to, must be lowercase.
   * @pre lowercaseRhs must be an all-lowercase string.
   * @tparam T The type of the first string, must be \ref StringLike (have `size()` and `data()
   * methods).
   * @tparam U The type of the second string, must be \ref StringLike (have `size()` and `data()
   * methods).
   * @return true If the \p lowercaseRhs is equal to the \p lhs, ignoring the case of the \p lhs.
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
   * Returns true if two strings are equal, optionally with a case-insensitive comparison.
   *
   * ```
   * StringUtils::Equals("Hello", "hello"); // false
   * StringUtils::Equals<StringComparison::IgnoreCase>("Hello", "hello"); // true
   * ```
   *
   * @param lhs The first string to compare.
   * @param rhs The second string to compare.
   * @tparam Comparison The comparison type to use, defaults to \ref StringComparison::Default.
   * @tparam T The type of the first string, must be \ref StringLike (have `size()` and `data()
   * methods).
   * @tparam U The type of the second string, must be \ref StringLike (have `size()` and `data()
   * methods).
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
   * Returns true if \p str starts with \p otherStr.
   *
   * ```
   * StringUtils::StartsWith("Hello", "He"); // true
   * StringUtils::StartsWith<StringComparison::IgnoreCase>("Hello", "he"); // true
   * ```
   *
   * @param str The string to check for a prefix.
   * @param otherStr The prefix to check for.
   * @tparam Comparison The comparison type to use, defaults to \ref StringComparison::Default.
   * @tparam T The type of the first string, must be \ref StringLike (have `size()` and `data()
   * methods).
   * @tparam U The type of the second string, must be \ref StringLike (have `size()` and `data()
   * methods).
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
   * Returns true if \p str ends with \p otherStr.
   *
   * ```
   * StringUtils::EndsWith("Hello", "llo"); // true
   * StringUtils::EndsWith<StringComparison::IgnoreCase>("Hello", "LLO"); // true
   * ```
   *
   * @param str The string to check for a suffix.
   * @param otherStr The suffix to check for.
   * @tparam Comparison The comparison type to use, defaults to \ref StringComparison::Default.
   * @tparam T The type of the first string, must be \ref StringLike (have `size()` and `data()
   * methods).
   * @tparam U The type of the second string, must be \ref StringLike (have `size()` and `data()
   * methods).
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
   * Returns true if \p str contains \p otherStr.
   *
   * ```
   * StringUtils::Contains("Hello world", "ello"); // true
   * StringUtils::Contains<StringComparison::IgnoreCase>("Hello world", "ELLO"); // true
   * ```
   *
   * @param str The string to check for a suffix.
   * @param otherStr The suffix to check for.
   * @tparam Comparison The comparison type to use, defaults to \ref StringComparison::Default.
   * @tparam T The type of the first string, must be \ref StringLike (have `size()` and `data()
   * methods).
   * @tparam U The type of the second string, must be \ref StringLike (have `size()` and `data()
   * methods).
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
   * Splits a string by a given character, returning a range of the split strings as a
   * `std::string_view`.
   *
   * ```
   * for (auto&& str : StringUtils::Split("a,b,c", ',')) {
   *   // ...
   * }
   * ```
   *
   * @tparam T The string type to split.
   * @param str The string to split.
   * @param ch The character to split by.
   * @tparam T The type of the string, must be \ref StringLike (have `size()` and `data() methods).
   * @return A vector of the split string views.
   */
  template <StringLike T>
  static std::vector<std::string_view> Split(const T& str, char ch = ' ') {
    // Ideally this would return a range directly, but if we try to move the ranges::to_vector call
    // outside this function, it results in a compile error since the `.begin()` method cannot be
    // resolved.
    const std::string_view strView(str.data(), str.size());
    return strView | ranges::cpp20::views::split(ch) |
           ranges::cpp20::views::transform(
               [](auto&& rng) { return std::string_view(&*rng.begin(), ranges::distance(rng)); }) |
           ranges::cpp20::views::filter(StringViewIsNonEmpty) | ranges::to_vector;
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
   * least this size, e.g `std::min(lhsSize, rhsSize)`.
   * @tparam Comparison The comparison type to use.
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

  /**
   * Helper to filter out empty std::string_views, used by \ref Split.
   *
   * @param str The string view to check.
   */
  static bool StringViewIsNonEmpty(std::string_view str) { return !str.empty(); }
};

}  // namespace donner
