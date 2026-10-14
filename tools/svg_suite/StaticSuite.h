#pragma once
/// @file
/// Plain SVG/PNG corpus discovery and synchronous renderer-independent execution.

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "tools/svg_suite/ImageCompare.h"

namespace donner::svg_suite {

struct RunProvenance;

/// A named directory containing recursively paired SVG and PNG files.
struct CorpusRoot {
  std::string name;
  std::filesystem::path path;
};

/// A discovered case. The ID encodes the corpus name and relative path without collisions.
struct StaticCase {
  std::string id;
  std::filesystem::path root;
  std::filesystem::path svgPath;
  std::filesystem::path expectedPath;
  std::shared_ptr<const RunProvenance> provenance = nullptr;
};

/// Discovery is all-or-error; malformed corpora never produce partial successful runs.
struct DiscoveryResult {
  std::vector<StaticCase> cases;
  std::string error;
};

/// Find lowercase .svg files recursively under root/tests when present, otherwise under root.
/// Require a same-stem PNG and bounded regular inputs.
/// Reject missing/empty roots, duplicate names and symlink escapes. No directory symlinks followed.
/// @param roots Named roots, normally the upstream corpus and the extension corpus.
DiscoveryResult DiscoverCases(const std::vector<CorpusRoot>& roots);

/// Render callback receives the case and canonical reference dimensions. It must enforce its own
/// SVG resource policy and execution limits; this synchronous API does not sandbox renderer code.
using Renderer = std::function<ImageResult(const StaticCase&, int width, int height)>;

/// Measured case outcome. Errors and image mismatches both fail, but remain distinguishable.
struct CaseResult {
  bool passed = false;
  std::string error;
  std::optional<int> mismatchedPixels;
  std::filesystem::path artifactDirectory;
};

/// Render and compare once, recording JSON metrics and available PNGs in a fresh case directory.
/// Existing case directories fail closed to preserve earlier evidence. No golden update path
/// exists.
/// @param test Case obtained through discovery.
/// @param renderer Renderer callback with explicit success/error outcome.
/// @param artifactRoot Existing or newly created output root, outside the corpus.
/// @param options Reviewed pixelmatch policy; strict by default.
CaseResult RunCase(const StaticCase& test, const Renderer& renderer,
                   const std::filesystem::path& artifactRoot,
                   const ComparisonOptions& options = {});

}  // namespace donner::svg_suite
