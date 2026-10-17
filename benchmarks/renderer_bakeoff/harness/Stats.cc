#include "harness/Stats.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace donner::bakeoff {

SampleStats ComputeStats(std::span<const double> samples) {
  SampleStats stats;
  if (samples.empty()) {
    return stats;
  }
  std::vector<double> sorted(samples.begin(), samples.end());
  std::sort(sorted.begin(), sorted.end());
  const std::size_t n = sorted.size();
  stats.count = n;
  stats.min = sorted.front();
  stats.max = sorted.back();
  stats.median = n % 2 == 1 ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
  const std::size_t rank =
      static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(n)));  // 1-based
  stats.p95 = sorted[std::clamp<std::size_t>(rank, 1, n) - 1];
  stats.mean = std::accumulate(sorted.begin(), sorted.end(), 0.0) / static_cast<double>(n);
  if (n > 1) {
    double sumSquares = 0.0;
    for (const double value : sorted) {
      sumSquares += (value - stats.mean) * (value - stats.mean);
    }
    stats.stddev = std::sqrt(sumSquares / static_cast<double>(n - 1));
  }
  return stats;
}

}  // namespace donner::bakeoff
