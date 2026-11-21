#include "donner/base/tests/ScopedCommaDecimalGlobalLocale.h"

#include <string>

namespace donner {
namespace {

// libstdc++ inspects this facet's type information when installing its dual-ABI twin.
class CommaDecimal final : public std::numpunct<char> {
protected:
  char do_decimal_point() const override { return ','; }
  char do_thousands_sep() const override { return '.'; }
  std::string do_grouping() const override { return "\3"; }
};

}  // namespace

ScopedCommaDecimalGlobalLocale::ScopedCommaDecimalGlobalLocale()
    : previous_(std::locale::global(std::locale(std::locale::classic(), new CommaDecimal))) {}

ScopedCommaDecimalGlobalLocale::~ScopedCommaDecimalGlobalLocale() {
  std::locale::global(previous_);
}

}  // namespace donner
