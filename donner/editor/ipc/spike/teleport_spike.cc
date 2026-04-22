/// @file
/// M0.1 feasibility-spike driver. Round-trips a RenderRequest through the
/// reflection-driven codec, prints per-encode / per-decode timings, and runs
/// a negative-decode test. See SPIKE_NOTES.md for build / run instructions.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "render_request.h"
#include "teleport_codec.h"

namespace {

using donner::teleport::Decode;
using donner::teleport::DecodeError;
using donner::teleport::Encode;
using donner::teleport::RenderRequest;
using donner::teleport::Vector2d;

constexpr int kIters = 100'000;

bool equals(const RenderRequest& a, const RenderRequest& b) {
  return a.width == b.width && a.height == b.height && a.svg_source == b.svg_source &&
         a.cursor.x == b.cursor.x && a.cursor.y == b.cursor.y;
}

long long medianNs(std::vector<long long>& samples) {
  std::nth_element(samples.begin(), samples.begin() + samples.size() / 2, samples.end());
  return samples[samples.size() / 2];
}

}  // namespace

int main() {
  RenderRequest req{
      .width = 1920,
      .height = 1080,
      .svg_source =
          R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
  <circle cx="50" cy="50" r="40" fill="red"/>
</svg>)SVG",
      .cursor = Vector2d{123.5, 456.75},
  };

  // Warm-up + correctness check.
  auto bytes = Encode(req);
  auto decoded = Decode<RenderRequest>(bytes);
  const bool match = decoded.has_value() && equals(req, *decoded);

  std::printf("Teleport spike — RenderRequest round-trip\n");
  std::printf("Encoded: %zu bytes\n", bytes.size());
  std::printf("Round-trip: %s\n", match ? "MATCH" : "FAIL");

  // Per-iteration timings. We use steady_clock around a single op so the
  // median absorbs allocator jitter from std::vector<std::byte> growth.
  std::vector<long long> encNs(kIters);
  for (int i = 0; i < kIters; ++i) {
    auto t0 = std::chrono::steady_clock::now();
    auto out = Encode(req);
    auto t1 = std::chrono::steady_clock::now();
    encNs[i] = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    asm volatile("" : : "r,m"(out.data()) : "memory");  // prevent DCE
  }

  std::vector<long long> decNs(kIters);
  for (int i = 0; i < kIters; ++i) {
    auto t0 = std::chrono::steady_clock::now();
    auto out = Decode<RenderRequest>(bytes);
    auto t1 = std::chrono::steady_clock::now();
    decNs[i] = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    if (!out.has_value()) std::abort();
  }

  std::printf("Encode: %lld ns/op (median over %dk iters)\n", medianNs(encNs), kIters / 1000);
  std::printf("Decode: %lld ns/op (median over %dk iters)\n", medianNs(decNs), kIters / 1000);

  // Negative test: feed a deliberately-truncated buffer. The decoder must
  // return an error rather than crash or read past the end. Truncate to one
  // byte past the u32 length prefix of svg_source — that guarantees the
  // string read tries to grab more bytes than remain.
  std::vector<std::byte> truncated(bytes.begin(), bytes.begin() + 12);
  auto bad = Decode<RenderRequest>(truncated);
  if (bad.has_value()) {
    std::printf("Negative test: FAIL (decoder accepted truncated input)\n");
    return 1;
  }
  std::printf("Negative test: OK (DecodeError=%d)\n", static_cast<int>(bad.error()));

  return match ? 0 : 1;
}
