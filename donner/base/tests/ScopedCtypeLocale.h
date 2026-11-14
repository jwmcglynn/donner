#pragma once
/// @file

#include <gtest/gtest.h>

#include <array>
#include <clocale>
#include <string>

namespace donner {

/**
 * Switches the process C locale's character classification (`LC_CTYPE`) for the lifetime of the
 * object and restores the previous locale afterwards.
 *
 * Use through \ref ForEachCtypeLocaleForTesting to show that text handling does not depend on the
 * locale:
 * in Latin-1 locales, and in UTF-8 locales on macOS, `std::tolower` and `std::isspace` treat bytes
 * above 0x7F as letters or spaces. A locale that is not installed leaves the current one active.
 */
class ScopedCtypeLocale {
public:
  /**
   * Activates \p name for `LC_CTYPE` if it is installed.
   *
   * @param name Locale name, for example "en_US.UTF-8".
   */
  explicit ScopedCtypeLocale(const char* name) : previous_(std::setlocale(LC_CTYPE, nullptr)) {
    applied_ = std::setlocale(LC_CTYPE, name) != nullptr;
  }

  ~ScopedCtypeLocale() { std::setlocale(LC_CTYPE, previous_.c_str()); }

  ScopedCtypeLocale(const ScopedCtypeLocale&) = delete;
  ScopedCtypeLocale& operator=(const ScopedCtypeLocale&) = delete;

  /// Whether the requested locale is installed and now active.
  bool applied() const { return applied_; }

private:
  std::string previous_;
  bool applied_ = false;
};

/// Locales to run locale-independence checks under. Only those installed on the host take effect.
inline constexpr std::array<const char*, 4> kCtypeLocalesForTesting = {
    "C", "en_US.UTF-8", "en_US.ISO8859-1", "en_US.ISO-8859-1"};

/**
 * Runs \p check once under each of \ref kCtypeLocalesForTesting, with a gtest trace naming the
 * locale and whether it is installed. The red result only shows on hosts with a locale whose
 * `tolower` or `isspace` treats bytes above 0x7F as letters or spaces, such as macOS.
 *
 * @param check Callable that makes the assertions.
 */
template <typename Fn>
void ForEachCtypeLocaleForTesting(const Fn& check) {
  for (const char* locale : kCtypeLocalesForTesting) {
    const ScopedCtypeLocale scopedLocale(locale);
    SCOPED_TRACE(testing::Message()
                 << "LC_CTYPE=" << locale << (scopedLocale.applied() ? "" : " (not installed)"));
    check();
  }
}

}  // namespace donner
