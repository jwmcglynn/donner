#include "donner/svg/renderer/benchmarks/ProcStatusParser.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

namespace donner::benchmarks {
namespace {

TEST(ProcStatusParser, ParsesWhitespaceAndKilobyteSuffix) {
  EXPECT_EQ(ParseProcStatusKilobytes("VmRSS:\t  12345 kB", "VmRSS:"),
            std::optional<std::uint64_t>(12345u));
  EXPECT_EQ(ParseProcStatusKilobytes("VmHWM: 0 kB\r", "VmHWM:"), std::optional<std::uint64_t>(0u));
}

TEST(ProcStatusParser, RequiresExactFieldAndUnit) {
  EXPECT_EQ(ParseProcStatusKilobytes("VmHWM: 123 kB", "VmRSS:"), std::nullopt);
  EXPECT_EQ(ParseProcStatusKilobytes("VmRSS: 123 MB", "VmRSS:"), std::nullopt);
  EXPECT_EQ(ParseProcStatusKilobytes("VmRSS: 123 kB trailing", "VmRSS:"), std::nullopt);
}

TEST(ProcStatusParser, RejectsMissingNegativeAndOverflowingValues) {
  EXPECT_EQ(ParseProcStatusKilobytes("VmRSS: kB", "VmRSS:"), std::nullopt);
  EXPECT_EQ(ParseProcStatusKilobytes("VmRSS: -1 kB", "VmRSS:"), std::nullopt);
  EXPECT_EQ(ParseProcStatusKilobytes("VmRSS: 18446744073709551616 kB", "VmRSS:"), std::nullopt);
}

TEST(ProcStatusParser, RejectsEmptyField) {
  EXPECT_EQ(ParseProcStatusKilobytes("VmRSS: 123 kB", ""), std::nullopt);
}

}  // namespace
}  // namespace donner::benchmarks
