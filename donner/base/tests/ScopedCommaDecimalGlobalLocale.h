#pragma once
/// @file

#include <locale>

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
  ScopedCommaDecimalGlobalLocale();
  ~ScopedCommaDecimalGlobalLocale();

  ScopedCommaDecimalGlobalLocale(const ScopedCommaDecimalGlobalLocale&) = delete;
  ScopedCommaDecimalGlobalLocale& operator=(const ScopedCommaDecimalGlobalLocale&) = delete;

private:
  std::locale previous_;
};

}  // namespace donner
