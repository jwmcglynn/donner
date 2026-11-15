#pragma once
/// @file

#include <locale>
#include <string>

namespace donner {

/**
 * Installs, for the lifetime of the object, a C++ global locale that formats numbers the way many
 * European locales do: ',' as the decimal point and '.' grouping thousands, so 1000.5 streams as
 * "1.000,5". Streams constructed while it is active pick it up unless they imbue another locale.
 *
 * The locale is built from a custom `std::numpunct` facet, so it does not depend on which named
 * locales the host has installed.
 */
class ScopedCommaDecimalGlobalLocale {
public:
  ScopedCommaDecimalGlobalLocale()
      : previous_(std::locale::global(std::locale(std::locale::classic(), new CommaDecimal))) {}

  ~ScopedCommaDecimalGlobalLocale() { std::locale::global(previous_); }

  ScopedCommaDecimalGlobalLocale(const ScopedCommaDecimalGlobalLocale&) = delete;
  ScopedCommaDecimalGlobalLocale& operator=(const ScopedCommaDecimalGlobalLocale&) = delete;

private:
  /// Number punctuation with a decimal comma and dot-grouped thousands.
  class CommaDecimal final : public std::numpunct<char> {
  protected:
    char do_decimal_point() const override { return ','; }
    char do_thousands_sep() const override { return '.'; }
    std::string do_grouping() const override { return "\3"; }
  };

  std::locale previous_;
};

}  // namespace donner
