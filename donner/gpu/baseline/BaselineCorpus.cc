#include "donner/gpu/baseline/BaselineCorpus.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <span>
#include <string>
#include <utility>

#include "donner/svg/renderer/geode/GeodePathEncoder.h"

namespace donner::gpu::baseline {
namespace {

using donner::geode::EncodedPath;
using donner::geode::GeodePathEncoder;
using tests::BaselinePathSpec;

/// A closed polygon from literal vertices, so the corpus never depends on a transcendental
/// function whose last bit may differ between platform math libraries.
Path Polygon(std::span<const Vector2d> points) {
  PathBuilder builder;
  builder.moveTo(points[0]);
  for (size_t i = 1; i < points.size(); ++i) {
    builder.lineTo(points[i]);
  }
  builder.closePath();
  return builder.build();
}

Path Rectangle(double left, double top, double right, double bottom) {
  const std::array<Vector2d, 4> corners = {Vector2d(left, top), Vector2d(right, top),
                                           Vector2d(right, bottom), Vector2d(left, bottom)};
  return Polygon(corners);
}

/// Two overlapping self-intersecting hexagrams drawn with opposite fill rules, so the frozen
/// set pins the winding-count fold and the even-odd fold against the same geometry.
std::vector<BaselinePathSpec> FillRulePairPaths() {
  const std::array<Vector2d, 6> left = {Vector2d(24.0, 40.0),   Vector2d(120.0, 40.0),
                                        Vector2d(40.0, 176.0),  Vector2d(88.0, 24.0),
                                        Vector2d(136.0, 176.0), Vector2d(24.0, 88.0)};
  const std::array<Vector2d, 6> right = {Vector2d(120.0, 80.0),  Vector2d(216.0, 80.0),
                                         Vector2d(136.0, 216.0), Vector2d(184.0, 64.0),
                                         Vector2d(232.0, 216.0), Vector2d(120.0, 128.0)};
  return {BaselinePathSpec{Polygon(left), css::RGBA(220, 60, 20, 200), FillRule::NonZero},
          BaselinePathSpec{Polygon(right), css::RGBA(20, 90, 220, 200), FillRule::EvenOdd}};
}

/// Axis-aligned translucent rectangles on integral device pixels: the coverage term is exactly
/// one over the interior, so any difference here is a blend or premultiplication difference and
/// not a coverage difference.
std::vector<BaselinePathSpec> AlphaStackPaths() {
  return {BaselinePathSpec{Rectangle(32.0, 32.0, 160.0, 160.0), css::RGBA(255, 0, 0, 96),
                           FillRule::NonZero},
          BaselinePathSpec{Rectangle(96.0, 64.0, 224.0, 192.0), css::RGBA(0, 255, 0, 96),
                           FillRule::NonZero},
          BaselinePathSpec{Rectangle(64.0, 96.0, 192.0, 224.0), css::RGBA(0, 0, 255, 96),
                           FillRule::NonZero}};
}

/// A cubic loop whose extrema fall strictly inside the segments, forcing a monotone split on
/// both the horizontal and the vertical ray rather than only at segment joins.
std::vector<BaselinePathSpec> CubicExtremaPaths() {
  PathBuilder builder;
  builder.moveTo(Vector2d(48.0, 128.0));
  builder.curveTo(Vector2d(48.0, 16.0), Vector2d(208.0, 16.0), Vector2d(208.0, 128.0));
  builder.curveTo(Vector2d(208.0, 240.0), Vector2d(48.0, 240.0), Vector2d(48.0, 128.0));
  builder.closePath();

  PathBuilder inner;
  inner.moveTo(Vector2d(128.0, 72.0));
  inner.curveTo(Vector2d(24.0, 96.0), Vector2d(232.0, 160.0), Vector2d(128.0, 184.0));
  inner.curveTo(Vector2d(96.0, 160.0), Vector2d(160.0, 96.0), Vector2d(128.0, 72.0));
  inner.closePath();

  return {BaselinePathSpec{builder.build(), css::RGBA(40, 140, 200, 255), FillRule::NonZero},
          BaselinePathSpec{inner.build(), css::RGBA(250, 220, 40, 220), FillRule::EvenOdd}};
}

/// A tall zigzag that spreads its curves across the full band grid on one axis while collapsing
/// them onto few bands on the other, pinning the band-packing counters at an asymmetric shape.
std::vector<BaselinePathSpec> BandGridStressPaths() {
  PathBuilder builder;
  builder.moveTo(Vector2d(20.0, 16.0));
  for (int step = 0; step < 24; ++step) {
    const double y = 16.0 + step * 9.0;
    const double x = (step % 2 == 0) ? 220.0 : 24.0;
    builder.quadTo(Vector2d(x, y + 2.0), Vector2d(x, y + 9.0));
  }
  builder.lineTo(Vector2d(20.0, 240.0));
  builder.closePath();
  return {BaselinePathSpec{builder.build(), css::RGBA(90, 200, 120, 255), FillRule::NonZero}};
}

/// Degenerate input: an empty path, a lone move, and a zero-area segment. All three must encode
/// to the `Empty` outcome and draw nothing.
std::vector<BaselinePathSpec> DegeneratePaths() {
  PathBuilder lone;
  lone.moveTo(Vector2d(64.0, 64.0));

  PathBuilder sliver;
  sliver.moveTo(Vector2d(32.0, 200.0));
  sliver.lineTo(Vector2d(200.0, 200.0));
  sliver.lineTo(Vector2d(32.0, 200.0));
  sliver.closePath();

  return {BaselinePathSpec{Path(), css::RGBA(255, 255, 255, 255), FillRule::NonZero},
          BaselinePathSpec{lone.build(), css::RGBA(255, 255, 255, 255), FillRule::NonZero},
          BaselinePathSpec{sliver.build(), css::RGBA(255, 255, 255, 255), FillRule::NonZero}};
}

/// Out-of-range input: coordinates past the float range the encoder admits. The frozen outcome
/// is the encoder's fail-closed rejection, which is a public error outcome of the current
/// implementation and therefore part of what a replacement must reproduce.
std::vector<BaselinePathSpec> RejectedPaths() {
  const double huge = std::numeric_limits<double>::max();
  const std::array<Vector2d, 3> triangle = {Vector2d(0.0, 0.0), Vector2d(huge, 0.0),
                                            Vector2d(0.0, huge)};

  PathBuilder infinite;
  infinite.moveTo(Vector2d(0.0, 0.0));
  infinite.lineTo(Vector2d(std::numeric_limits<double>::infinity(), 32.0));
  infinite.lineTo(Vector2d(32.0, 32.0));
  infinite.closePath();

  return {BaselinePathSpec{Polygon(triangle), css::RGBA(255, 0, 255, 255), FillRule::NonZero},
          BaselinePathSpec{infinite.build(), css::RGBA(255, 0, 255, 255), FillRule::NonZero}};
}

AxisCounters ToAxisCounters(const EncodedPath::AxisStats& stats) {
  AxisCounters counters;
  counters.canonicalCurveCount = stats.canonicalCurveCount;
  counters.curveReferenceCount = stats.curveReferenceCount;
  counters.omittedParallelCurves = stats.omittedParallelCurves;
  counters.gridBandCount = stats.gridBandCount;
  counters.nonemptyBandCount = stats.nonemptyBandCount;
  counters.maxCurvesPerBand = stats.maxCurvesPerBand;
  counters.p95CurvesPerBand = stats.p95CurvesPerBand;
  return counters;
}

std::string OutcomeName(EncodedPath::Outcome outcome) {
  switch (outcome) {
    case EncodedPath::Outcome::Empty: return "Empty";
    case EncodedPath::Outcome::Ready: return "Ready";
    case EncodedPath::Outcome::Rejected: return "Rejected";
  }
  return "Unknown";
}

PathCounters ToPathCounters(const EncodedPath& encoded) {
  PathCounters counters;
  counters.outcome = OutcomeName(encoded.outcome);
  counters.horizontal = ToAxisCounters(encoded.stats.horizontal);
  counters.vertical = ToAxisCounters(encoded.stats.vertical);
  counters.boundingVertexCount = encoded.boundingVertexCount;
  counters.boundingDrawVertexCount = encoded.boundingDrawVertexCount();
  counters.geometryItemCount = static_cast<uint64_t>(encoded.geometryItemCount());
  if (encoded.outcome == EncodedPath::Outcome::Ready) {
    counters.boundsMinX = encoded.pathBounds.topLeft.x;
    counters.boundsMinY = encoded.pathBounds.topLeft.y;
    counters.boundsMaxX = encoded.pathBounds.bottomRight.x;
    counters.boundsMaxY = encoded.pathBounds.bottomRight.y;
  }
  return counters;
}

/// Fixed six-decimal form. The frozen manifest must not depend on a default float formatter.
std::string FixedNumber(double value) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.6f", value);
  return buffer;
}

void AppendAxis(std::string& out, std::string_view key, const AxisCounters& axis,
                std::string_view indent) {
  out += indent;
  out += "\"";
  out += key;
  out += "\": {";
  out += "\"canonicalCurveCount\": " + std::to_string(axis.canonicalCurveCount);
  out += ", \"curveReferenceCount\": " + std::to_string(axis.curveReferenceCount);
  out += ", \"omittedParallelCurves\": " + std::to_string(axis.omittedParallelCurves);
  out += ", \"gridBandCount\": " + std::to_string(axis.gridBandCount);
  out += ", \"nonemptyBandCount\": " + std::to_string(axis.nonemptyBandCount);
  out += ", \"maxCurvesPerBand\": " + std::to_string(axis.maxCurvesPerBand);
  out += ", \"p95CurvesPerBand\": " + std::to_string(axis.p95CurvesPerBand);
  out += "},\n";
}

void AppendPath(std::string& out, const PathCounters& path) {
  out += "        {\n";
  out += "          \"outcome\": \"" + path.outcome + "\",\n";
  AppendAxis(out, "horizontal", path.horizontal, "          ");
  AppendAxis(out, "vertical", path.vertical, "          ");
  out += "          \"boundingVertexCount\": " + std::to_string(path.boundingVertexCount) + ",\n";
  out += "          \"boundingDrawVertexCount\": " + std::to_string(path.boundingDrawVertexCount) +
         ",\n";
  out += "          \"geometryItemCount\": " + std::to_string(path.geometryItemCount) + ",\n";
  out += "          \"bounds\": [" + FixedNumber(path.boundsMinX) + ", " +
         FixedNumber(path.boundsMinY) + ", " + FixedNumber(path.boundsMaxX) + ", " +
         FixedNumber(path.boundsMaxY) + "]\n";
  out += "        }";
}

void AppendScene(std::string& out, const SceneCounters& scene) {
  out += "    {\n";
  out += "      \"name\": \"" + scene.name + "\",\n";
  out += "      \"paths\": [\n";
  for (size_t i = 0; i < scene.paths.size(); ++i) {
    AppendPath(out, scene.paths[i]);
    out += (i + 1 == scene.paths.size()) ? "\n" : ",\n";
  }
  out += "      ]\n";
  out += "    }";
}

}  // namespace

std::vector<CorpusScene> Corpus() {
  std::vector<CorpusScene> corpus;
  corpus.push_back({"solid_fill_baseline",
                    "Translucent quadratic circle, self-intersecting even-odd star, and an "
                    "opaque cubic blob overlapping both.",
                    tests::BaselineScenePaths()});
  corpus.push_back({"fill_rule_pair",
                    "Two overlapping self-intersecting hexagrams, non-zero against even-odd.",
                    FillRulePairPaths()});
  corpus.push_back({"alpha_stack",
                    "Three axis-aligned translucent rectangles on integral device pixels.",
                    AlphaStackPaths()});
  corpus.push_back({"cubic_extrema", "Cubic loops whose extrema fall inside segments on both rays.",
                    CubicExtremaPaths()});
  corpus.push_back({"band_grid_stress",
                    "A 24-segment zigzag with an asymmetric horizontal and vertical band split.",
                    BandGridStressPaths()});
  corpus.push_back({"degenerate_paths",
                    "An empty path, a lone move, and a zero-area segment; all draw nothing.",
                    DegeneratePaths()});
  corpus.push_back({"rejected_out_of_range",
                    "Coordinates past the admitted float range; the encoder fails closed.",
                    RejectedPaths(), /*capturesPixels=*/false});
  return corpus;
}

std::vector<SceneCounters> ComputeCorpusCounters() {
  std::vector<SceneCounters> result;
  for (const CorpusScene& scene : Corpus()) {
    SceneCounters counters;
    counters.name = std::string(scene.name);
    for (const BaselinePathSpec& spec : scene.paths) {
      counters.paths.push_back(ToPathCounters(GeodePathEncoder::encode(spec.path, spec.rule)));
    }
    result.push_back(std::move(counters));
  }
  return result;
}

std::string CorpusCountersJson() {
  const std::vector<SceneCounters> scenes = ComputeCorpusCounters();
  std::string out;
  out += "{\n";
  out +=
      "  \"_comment\": \"Structural counters of the frozen GPU baseline corpus, derived on the "
      "CPU by the production path encoder. Regenerate with: bazel run "
      "//donner/gpu/baseline:dump_baseline_counters > "
      "donner/gpu/baseline/baselines/structural_counters.json\",\n";
  out += "  \"schemaVersion\": 1,\n";
  out += "  \"scenes\": [\n";
  for (size_t i = 0; i < scenes.size(); ++i) {
    AppendScene(out, scenes[i]);
    out += (i + 1 == scenes.size()) ? "\n" : ",\n";
  }
  out += "  ]\n";
  out += "}\n";
  return out;
}

}  // namespace donner::gpu::baseline
