#include "harness/Stats.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <vector>

namespace donner::bakeoff {
namespace {

using testing::AllOf;
using testing::DoubleNear;
using testing::Field;

TEST(StatsTest, EmptyInputIsAllZero) {
  const SampleStats stats = ComputeStats({});
  EXPECT_THAT(stats, AllOf(Field(&SampleStats::count, 0u), Field(&SampleStats::median, 0.0),
                           Field(&SampleStats::stddev, 0.0)));
}

TEST(StatsTest, OddCountUsesMiddleSampleAndNearestRankP95) {
  const std::vector<double> samples = {5, 1, 4, 2, 3};
  const SampleStats stats = ComputeStats(samples);
  EXPECT_THAT(stats, AllOf(Field(&SampleStats::count, 5u), Field(&SampleStats::min, 1.0),
                           Field(&SampleStats::median, 3.0), Field(&SampleStats::p95, 5.0),
                           Field(&SampleStats::max, 5.0), Field(&SampleStats::mean, 3.0),
                           Field(&SampleStats::stddev, DoubleNear(1.5811388, 1e-6))));
}

TEST(StatsTest, EvenCountAveragesMiddleSamples) {
  std::vector<double> samples;
  for (int i = 1; i <= 20; ++i) {
    samples.push_back(i);
  }
  const SampleStats stats = ComputeStats(samples);
  EXPECT_THAT(stats, AllOf(Field(&SampleStats::median, 10.5), Field(&SampleStats::p95, 19.0)));
}

}  // namespace
}  // namespace donner::bakeoff
