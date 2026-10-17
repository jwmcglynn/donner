#include "harness/SyntheticScenes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numbers>

namespace donner::bakeoff {

namespace {

/// Small deterministic generator; std distributions are not specified bit-exactly across
/// standard libraries, so scenes use this directly.
class Lcg {
public:
  explicit Lcg(std::uint64_t seed) : state_(seed) {}

  /// Uniform value in [0, 1).
  double next() {
    state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<double>(state_ >> 11) / static_cast<double>(1ULL << 53);
  }

  /// Uniform value in [lo, hi).
  double range(double lo, double hi) { return lo + (hi - lo) * next(); }

private:
  std::uint64_t state_;
};

/// Elements are emitted without whitespace between them: Donner's default parser limits count
/// whitespace text nodes toward its element budget, and the stress scenes stay within defaults.
std::string Header() {
  return "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1000\" height=\"1000\" "
         "viewBox=\"0 0 1000 1000\">\n<rect width=\"1000\" height=\"1000\" fill=\"#fdfcf7\"/>\n";
}

std::string Color(Lcg& rng) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x", static_cast<int>(rng.range(0, 256)),
                static_cast<int>(rng.range(0, 256)), static_cast<int>(rng.range(0, 256)));
  return buffer;
}

void Append(std::string& out, const char* format, auto... args) {
  char buffer[512];
  const int written = std::snprintf(buffer, sizeof(buffer), format, args...);
  if (written > 0) {
    out.append(buffer, static_cast<std::size_t>(std::min<int>(written, sizeof(buffer) - 1)));
  }
}

std::string ManyRects() {
  Lcg rng(1);
  std::string svg = Header();
  for (int i = 0; i < 7000; ++i) {
    Append(svg, "<rect x=\"%.2f\" y=\"%.2f\" width=\"%.2f\" height=\"%.2f\" fill=\"%s\"/>",
           rng.range(0, 990), rng.range(0, 990), rng.range(2, 14), rng.range(2, 14),
           Color(rng).c_str());
  }
  return svg + "</svg>\n";
}

std::string ManyCurves() {
  Lcg rng(2);
  std::string svg = Header();
  for (int i = 0; i < 2000; ++i) {
    const double x = rng.range(20, 980);
    const double y = rng.range(20, 980);
    const double s = rng.range(10, 40);
    Append(svg,
           "<path d=\"M%.2f %.2f C%.2f %.2f %.2f %.2f %.2f %.2f S%.2f %.2f %.2f %.2f Z\" "
           "fill=\"%s\" fill-opacity=\"0.6\"/>",
           x, y, x + rng.range(-s, s), y - s, x + s, y + rng.range(-s, s), x + s * 0.8, y + s,
           x - rng.range(0, s), y + s * 1.2, x - s * 0.5, y + s * 0.4, Color(rng).c_str());
  }
  return svg + "</svg>\n";
}

std::string LongPath() {
  std::string svg = Header();
  svg += "<path fill=\"none\" stroke=\"#1d3557\" stroke-width=\"1.5\" d=\"M500 500";
  constexpr int kSegments = 20000;
  for (int i = 1; i <= kSegments; ++i) {
    const double t = static_cast<double>(i) / kSegments;
    const double angle = t * 120.0 * std::numbers::pi;
    const double radius = 480.0 * t;
    Append(svg, " L%.2f %.2f", 500.0 + radius * std::cos(angle), 500.0 + radius * std::sin(angle));
  }
  return svg + "\"/>\n</svg>\n";
}

std::string ManyStrokes() {
  Lcg rng(3);
  std::string svg = Header();
  for (int i = 0; i < 2000; ++i) {
    const double x = rng.range(0, 1000);
    const double y = rng.range(0, 1000);
    Append(svg,
           "<path d=\"M%.2f %.2f Q%.2f %.2f %.2f %.2f T%.2f %.2f\" fill=\"none\" stroke=\"%s\" "
           "stroke-width=\"%.2f\" stroke-linejoin=\"round\" stroke-linecap=\"round\"/>",
           x, y, x + rng.range(-60, 60), y + rng.range(-60, 60), x + rng.range(-60, 60),
           y + rng.range(-60, 60), x + rng.range(-60, 60), y + rng.range(-60, 60),
           Color(rng).c_str(), rng.range(0.5, 6));
  }
  return svg + "</svg>\n";
}

std::string ManyGradients() {
  Lcg rng(4);
  std::string svg = Header();
  svg += "<defs>";
  for (int i = 0; i < 500; ++i) {
    Append(svg,
           "<linearGradient id=\"g%d\" x1=\"0\" y1=\"0\" x2=\"1\" y2=\"%.2f\"><stop offset=\"0\" "
           "stop-color=\"%s\"/><stop offset=\"1\" stop-color=\"%s\"/></linearGradient>",
           i, rng.range(0, 1), Color(rng).c_str(), Color(rng).c_str());
  }
  svg += "</defs>";
  for (int i = 0; i < 500; ++i) {
    Append(svg, "<circle cx=\"%.2f\" cy=\"%.2f\" r=\"%.2f\" fill=\"url(#g%d)\"/>",
           rng.range(0, 1000), rng.range(0, 1000), rng.range(8, 40), i);
  }
  return svg + "</svg>\n";
}

}  // namespace

std::vector<std::string_view> SyntheticSceneNames() {
  return {"stress_rects_7k", "stress_curves_2k", "stress_long_path_20k", "stress_strokes_2k",
          "stress_gradients_500"};
}

std::optional<std::string> GenerateSyntheticScene(std::string_view name) {
  if (name == "stress_rects_7k") {
    return ManyRects();
  }
  if (name == "stress_curves_2k") {
    return ManyCurves();
  }
  if (name == "stress_long_path_20k") {
    return LongPath();
  }
  if (name == "stress_strokes_2k") {
    return ManyStrokes();
  }
  if (name == "stress_gradients_500") {
    return ManyGradients();
  }
  return std::nullopt;
}

}  // namespace donner::bakeoff
