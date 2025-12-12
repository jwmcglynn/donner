#pragma once
/// @file
/// @brief ImGui-based transform inspector/editor for SVG transforms.

#include <optional>
#include <string>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/svg/parser/PathParser.h"
#include "donner/svg/parser/TransformParser.h"

namespace donner::experimental {

using donner::svg::PathSpline;
using donner::svg::parser::PathParser;
using donner::svg::parser::TransformParser;

/**
 * ImGui-based transform inspector/editor that visualizes parsed SVG transforms, decomposition
 * controls, comparison matrices, geometry overlays, and test exports.
 */
class TransformInspector {
public:
  TransformInspector() = default;

  /// Returns whether the inspector window should be rendered.
  bool isVisible() const { return state_.isVisible; }
  /// Sets the window visibility flag.
  void setVisible(bool visible) { state_.isVisible = visible; }

  /// Draws the inspector UI and updates internal state.
  void render();

private:
  /// Decomposed transform components used for UI editing and serialization.
  struct DecomposedTransform {
    /// Translation vector extracted from the current transform.
    Vector2d translation = Vector2d(0.0, 0.0);
    /// Scale factors extracted from the current transform.
    Vector2d scale = Vector2d(1.0, 1.0);
    /// Rotation value in degrees.
    double rotationDegrees = 0.0;
    /// Skew angle around the X axis in degrees.
    double skewXDegrees = 0.0;
    /// Skew angle around the Y axis in degrees.
    double skewYDegrees = 0.0;
    /// Indicates whether the decomposition is valid for the current transform.
    bool valid = false;
  };

  /// Parser options configured by the inspector UI.
  struct ParserToggles {
    /// Interpret transform angles as radians instead of degrees when enabled.
    bool anglesInRadians = false;
  };

  /// Options for computing and displaying a reference transform.
  struct ReferenceOptions {
    /// Enables side-by-side comparison against a degree-based reference parse.
    bool enabled = false;
  };

  /// Polyline used for geometry overlay rendering.
  struct Polyline {
    /// Sampled points for the path.
    std::vector<Vector2d> points;
    /// Whether the polyline represents a closed contour.
    bool closed = false;
  };

  /// Aggregate UI state persisted between frames.
  struct State {
    /// Controls visibility of the inspector window.
    bool isVisible = true;
    /// Raw transform string edited by the user.
    std::string transformString = "translate(30,20) rotate(30)";
    /// Rectangle geometry used when no path data is provided.
    Boxd rect = Boxd::FromXYWH(0.0, 0.0, 120.0, 80.0);
    /// Optional SVG path data for sampling instead of the rectangle.
    std::string pathData;
    /// Recent transform strings for quick recall.
    std::vector<std::string> recentTransforms;
    /// Cached decomposition synced with the raw transform string.
    DecomposedTransform decomposition;
    /// Parser settings chosen by the user.
    ParserToggles parserToggles;
    /// Reference comparison settings.
    ReferenceOptions referenceOptions;

    /// Adds the current transform string to history if non-empty and not duplicate.
    void rememberTransform();
  };

  /// Results produced after parsing and geometry sampling for the current state.
  struct Result {
    /// Whether the primary transform parsed successfully.
    bool parsed = false;
    /// Parsed transform when available.
    Transformd transform;
    /// Bounds of the original geometry.
    Boxd originalBounds;
    /// Bounds of the transformed geometry.
    Boxd transformedBounds;
    /// Error message when parsing fails.
    std::string error;
    /// Decomposition derived from the parsed transform.
    DecomposedTransform decomposition;
    /// Sampled original geometry polylines.
    std::vector<Polyline> geometry;
    /// Geometry after applying the parsed transform.
    std::vector<Polyline> transformedGeometry;
    /// Note describing how geometry was derived (e.g., fallback rectangle).
    std::string geometryNote;
    /// Whether the reference transform parsed successfully.
    bool referenceParsed = false;
    /// Reference transform used for diffing.
    Transformd referenceTransform;
    /// Bounds after applying the reference transform.
    Boxd referenceBounds;
    /// Geometry transformed by the reference parse.
    std::vector<Polyline> referenceGeometry;
    /// Error message when the reference parse fails.
    std::string referenceError;
  };

  /// Decomposes a transform into translation/scale/rotation/skew components.
  static std::optional<DecomposedTransform> decomposeTransform(const Transformd& transform);
  /// Serializes decomposition fields back into an SVG transform string.
  static std::string serializeDecomposition(const DecomposedTransform& decomposition,
                                            bool anglesInRadians);
  /// Escapes quotes and newlines for embedding in test snippets.
  static std::string escapeForSnippet(std::string_view value);
  /// Builds rectangle outline geometry used when no path data exists.
  static std::vector<Polyline> buildRectangleGeometry(const Boxd& rect);
  /// Samples path geometry into a polyline representation for overlay rendering.
  static std::vector<Polyline> samplePathGeometry(const PathSpline& path);
  /// Computes axis-aligned bounds of a set of polylines.
  static std::optional<Boxd> computeBounds(const std::vector<Polyline>& geometry);
  /// Applies a transform to every point in a set of polylines.
  static std::vector<Polyline> applyTransform(const std::vector<Polyline>& geometry,
                                              const Transformd& transform);

  /// Parses input, samples geometry, and generates the render-time result bundle.
  Result evaluate() const;
  /// Builds a gtest snippet from a successful parse and geometry sample.
  std::string buildTestSnippet(const Result& result) const;
  /// Replaces the current transform string with a generated value and updates history.
  void applyGeneratedTransform(const std::string& transform);
  /// Renders transform history recall UI.
  void drawTransformHistory();
  /// Renders rectangle and optional path inputs.
  void drawRectangleInputs();
  /// Renders parser option toggles.
  void drawParserOptions();
  /// Renders reference comparison toggles.
  void drawReferenceOptions();
  /// Renders decomposition sliders and handles synchronization back to the transform string.
  void drawDecomposition(Result& result);
  /// Renders edge-case transform generators.
  void drawEdgeCaseHelpers();
  /// Renders the test export controls and clipboard actions.
  void drawTestExport(const Result& result);
  /// Renders copy/reset actions for the transform string.
  void drawTransformActions();
  /// Renders inline usage notes.
  void drawHelpSection();
  /// Renders the geometry overlay preview.
  void drawGeometryOverlay(const Result& result);
  /// Renders parsing status, matrices, and bounds outputs.
  void drawParseResult(const Result& result);

  /// Resets state to defaults and clears derived data.
  void resetState();

  State state_;
};

}  // namespace donner::experimental
