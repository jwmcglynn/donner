#pragma once
/// @file

#include <clocale>
#include <string>

namespace donner {

/**
 * Switches the C library's `LC_NUMERIC` category to a comma-decimal locale for the lifetime of the
 * object, if one is installed, and restores the previous locale afterwards. Under it, `strtod` and
 * `printf` use ',' as the decimal point.
 *
 * Unlike the C++ global locale, the C locale cannot be built from a custom facet, so this depends
 * on the host's installed locales. Tests check `applied()` and skip visibly when none is found.
 */
class ScopedCommaDecimalNumericLocale {
public:
  ScopedCommaDecimalNumericLocale() : previous_(std::setlocale(LC_NUMERIC, nullptr)) {
    for (const char* name : {"de_DE.UTF-8", "de_DE.ISO8859-1", "fr_FR.UTF-8", "de_DE"}) {
      if (std::setlocale(LC_NUMERIC, name) != nullptr) {
        applied_ = true;
        return;
      }
    }
  }

  ~ScopedCommaDecimalNumericLocale() { std::setlocale(LC_NUMERIC, previous_.c_str()); }

  ScopedCommaDecimalNumericLocale(const ScopedCommaDecimalNumericLocale&) = delete;
  ScopedCommaDecimalNumericLocale& operator=(const ScopedCommaDecimalNumericLocale&) = delete;

  /// Whether a comma-decimal locale is installed and now active.
  bool applied() const { return applied_; }

private:
  std::string previous_;
  bool applied_ = false;
};

}  // namespace donner
