#pragma once
/// @file

#include <gtest/gtest.h>

#include <array>
#include <clocale>
#include <string>
#include <string_view>

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

/// Locales to run locale-independence checks under; the non-C ones are used where installed.
inline constexpr std::array<const char*, 4> kCtypeLocalesForTesting = {
    "C", "en_US.UTF-8", "en_US.ISO8859-1", "en_US.ISO-8859-1"};

/**
 * Runs \p check once under each installed locale of \ref kCtypeLocalesForTesting, with a gtest
 * trace naming the locale. Locales that are not installed are listed in the test's
 * `ctype_locales_not_installed` property. If no locale other than "C" is installed, the test is
 * marked skipped after the C-locale check, since locale independence was not exercised.
 *
 * A check only fails on hosts with a locale whose `tolower` or `isspace` treats bytes above 0x7F as
 * letters or spaces, such as macOS; glibc UTF-8 locales leave those bytes alone.
 *
 * @param check Callable that makes the assertions.
 */
template <typename Fn>
void ForEachCtypeLocaleForTesting(const Fn& check) {
  std::string notInstalled;
  bool ranNonClassicLocale = false;
  for (const char* locale : kCtypeLocalesForTesting) {
    const ScopedCtypeLocale scopedLocale(locale);
    if (!scopedLocale.applied()) {
      notInstalled += notInstalled.empty() ? locale : std::string(", ") + locale;
      continue;
    }
    ranNonClassicLocale |= std::string_view(locale) != "C";
    SCOPED_TRACE(testing::Message() << "LC_CTYPE=" << locale);
    check();
  }
  if (!notInstalled.empty()) {
    testing::Test::RecordProperty("ctype_locales_not_installed", notInstalled);
  }
  if (!ranNonClassicLocale) {
    GTEST_SKIP() << "No locale other than C is installed (missing: " << notInstalled
                 << "), so locale independence was not exercised";
  }
}

}  // namespace donner
