/// @file
/// Wall-clock style time for the CSS cascade cases the performance goals are stated against. Runs
/// in the manual perf lane; times are recorded as test properties, not asserted.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <functional>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

#include "donner/base/tests/RunfileGate.h"
#include "donner/benchmarks/CssCascadeScenes.h"

namespace donner::benchmarks {
namespace {

std::string ReadRunfileOrFail(std::string_view path) {
  tests::RequiredRunfile file = tests::ReadRequiredRunfile(path);
  EXPECT_THAT(file.error, ::testing::IsEmpty());
  return std::move(file.contents);
}

/// Record the median, minimum and maximum of \p timing as test properties named after \p prefix.
void RecordTiming(const std::string& prefix, const TimingSummary& timing) {
  ::testing::Test::RecordProperty(prefix + "_median_ms", std::to_string(timing.medianMs));
  ::testing::Test::RecordProperty(prefix + "_min_ms", std::to_string(timing.minMs));
  ::testing::Test::RecordProperty(prefix + "_max_ms", std::to_string(timing.maxMs));
  std::printf("%s: median %.3f ms (min %.3f, max %.3f, n=%zu)\n", prefix.c_str(), timing.medianMs,
              timing.minMs, timing.maxMs, timing.samples);
}

struct WallclockCase {
  std::string name;
  std::function<std::optional<CascadeMeasurement>()> measure;
};

std::ostream& operator<<(std::ostream& os, const WallclockCase& value) {
  return os << value.name;
}

class CssCascadeWallclockTest : public ::testing::TestWithParam<WallclockCase> {};

TEST_P(CssCascadeWallclockTest, RecordsStyleTime) {
  const std::optional<CascadeMeasurement> measurement = GetParam().measure();
  ASSERT_THAT(measurement, ::testing::Optional(::testing::_));
  RecordTiming("null_frame", measurement->nullFrame);
  RecordTiming("style", measurement->style);
}

INSTANTIATE_TEST_SUITE_P(
    Cases, CssCascadeWallclockTest,
    ::testing::Values(WallclockCase{"SplashFirstFrame",
                                    [] {
                                      return MeasureCascadeLoad(ReadRunfileOrFail(kSplashPath),
                                                                kLoadRepetitions,
                                                                /*withRenderer=*/false);
                                    }},
                      WallclockCase{"SplashClassEditFrame",
                                    [] {
                                      return MeasureCascadeEdit(
                                          ReadRunfileOrFail(kSplashPath), SplashEditTarget(),
                                          CascadeEdit::Class, kEditRepetitions,
                                          /*withRenderer=*/false);
                                    }},
                      WallclockCase{"BootstrapIconsFirstFrame",
                                    [] {
                                      return MeasureCascadeLoad(
                                          ReadRunfileOrFail(kBootstrapIconsPath), kLoadRepetitions,
                                          /*withRenderer=*/false);
                                    }},
                      WallclockCase{"Synthetic2000x200FirstFrame",
                                    [] {
                                      return MeasureCascadeLoad(MakeSyntheticDocument(),
                                                                kLoadRepetitions,
                                                                /*withRenderer=*/false);
                                    }}),
    [](const ::testing::TestParamInfo<WallclockCase>& info) { return info.param.name; });

}  // namespace
}  // namespace donner::benchmarks
