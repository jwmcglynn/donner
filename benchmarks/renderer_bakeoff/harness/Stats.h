#pragma once
/// @file
/// Summary statistics over frame-time samples.

#include <cstddef>
#include <span>

namespace donner::bakeoff {

/// Summary of a sample set, in the samples' unit.
struct SampleStats {
  std::size_t count = 0;  //!< Number of samples.
  double min = 0.0;       //!< Smallest sample.
  double median = 0.0;    //!< Median (mean of the two middle samples for an even count).
  double p95 = 0.0;       //!< 95th percentile by the nearest-rank method.
  double max = 0.0;       //!< Largest sample.
  double mean = 0.0;      //!< Arithmetic mean.
  double stddev = 0.0;    //!< Sample standard deviation (n - 1), zero for fewer than two samples.
};

/// Computes \ref SampleStats; an empty input yields all zeros.
/// @param samples Samples to summarize.
SampleStats ComputeStats(std::span<const double> samples);

}  // namespace donner::bakeoff
