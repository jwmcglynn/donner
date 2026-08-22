/// @file
/// @brief Allocation and peak-live-memory gates for SVG parsing.

#include <gtest/gtest.h>

#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/benchmarks/AllocationTracker.h"

namespace donner::benchmarks {
namespace {

using allocations::Snapshot;
using svg::parser::SVGParser;

struct ParseAllocationBudget {
  std::uint64_t allocationCalls;
  std::uint64_t allocationBytes;
  std::uint64_t liveBytes;
  std::uint64_t peakLiveBytes;
};

/// Parent baseline plus portability headroom for standard-library layout differences.
constexpr ParseAllocationBudget kRepeatedAttributesBudget = {
    .allocationCalls = 16'500,
    .allocationBytes = 7'500'000,
    .liveBytes = 7'100'000,
    .peakLiveBytes = 7'100'000,
};

/// Parent baseline plus portability headroom for standard-library layout differences.
constexpr ParseAllocationBudget kDonnerSplashBudget = {
    .allocationCalls = 26'500,
    .allocationBytes = 10'500'000,
    .liveBytes = 9'500'000,
    .peakLiveBytes = 9'500'000,
};

std::optional<std::string> ReadFile(std::string_view path) {
  std::ifstream file(std::string(path), std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  std::ostringstream contents;
  contents << file.rdbuf();
  return contents.str();
}

std::string RepeatedAttributesSvg() {
  std::string source = R"(<svg xmlns="http://www.w3.org/2000/svg">)";
  for (int i = 0; i < 256; ++i) {
    source +=
        R"(<rect data-repeated-long-attribute="the-same-long-attribute-value-for-every-rect" )";
    source += "id=\"rect-" + std::to_string(i) + "\" x=\"1\" y=\"2\" width=\"3\" height=\"4\"/>";
  }
  source += "</svg>";
  return source;
}

std::optional<Snapshot> MeasureParse(std::string_view source, std::string& error) {
  ParseWarningSink warningSink = ParseWarningSink::Disabled();
  SVGParser::Options options;
  options.disableUserAttributes = false;

  allocations::Scope scope;
  auto parsed = SVGParser::ParseSVG(source, warningSink, options);
  if (parsed.hasError()) {
    error = std::string(parsed.error().reason);
    return std::nullopt;
  }

  svg::SVGDocument document = std::move(parsed.result());
  Snapshot snapshot = scope.stop();
  (void)document;
  return snapshot;
}

void PrintSnapshot(std::string_view name, const Snapshot& snapshot) {
  std::fprintf(stderr,
               "SVG_PARSE_ALLOC name=%.*s calls=%" PRIu64 " bytes=%" PRIu64 " frees=%" PRIu64
               " live_bytes=%" PRIu64 " peak_live_bytes=%" PRIu64 "\n",
               static_cast<int>(name.size()), name.data(), snapshot.allocationCalls,
               snapshot.allocationBytes, snapshot.freeCalls, snapshot.liveBytes,
               snapshot.peakLiveBytes);
}

void ExpectWithinBudget(const Snapshot& snapshot, const ParseAllocationBudget& budget) {
  EXPECT_LE(snapshot.allocationCalls, budget.allocationCalls);
  EXPECT_LE(snapshot.allocationBytes, budget.allocationBytes);
  EXPECT_LE(snapshot.liveBytes, budget.liveBytes);
  EXPECT_LE(snapshot.peakLiveBytes, budget.peakLiveBytes);
  EXPECT_GE(snapshot.peakLiveBytes, snapshot.liveBytes);
}

TEST(AllocationTrackerTest, TracksLiveAndPeakRequestedBytes) {
  allocations::Scope scope;
  void* first = ::operator new(64);
  void* second = ::operator new(32);
  ::operator delete(first);
  const Snapshot snapshot = scope.stop();
  ::operator delete(second);

  EXPECT_EQ(snapshot.allocationCalls, 2u);
  EXPECT_EQ(snapshot.allocationBytes, 96u);
  EXPECT_EQ(snapshot.freeCalls, 1u);
  EXPECT_EQ(snapshot.liveBytes, 32u);
  EXPECT_EQ(snapshot.peakLiveBytes, 96u);
}

TEST(SvgParseAllocationTest, RepeatedAttributes) {
  const std::string source = RepeatedAttributesSvg();
  std::string error;
  const std::optional<Snapshot> snapshot = MeasureParse(source, error);
  ASSERT_TRUE(snapshot.has_value()) << error;
  PrintSnapshot("repeated_attributes", *snapshot);
  ExpectWithinBudget(*snapshot, kRepeatedAttributesBudget);
}

TEST(SvgParseAllocationTest, DonnerSplash) {
  const std::optional<std::string> source = ReadFile("donner_splash.svg");
  ASSERT_TRUE(source.has_value()) << "donner_splash.svg not found in runfiles";
  ASSERT_FALSE(source->empty());

  std::string error;
  const std::optional<Snapshot> snapshot = MeasureParse(*source, error);
  ASSERT_TRUE(snapshot.has_value()) << error;
  PrintSnapshot("donner_splash", *snapshot);
  ExpectWithinBudget(*snapshot, kDonnerSplashBudget);
}

}  // namespace
}  // namespace donner::benchmarks
