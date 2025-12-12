#include "experimental/viewer/TransformInspector.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <iomanip>
#include <optional>
#include <random>
#include <sstream>
#include <string_view>

#include "donner/base/MathUtils.h"
#include "donner/svg/parser/PathParser.h"
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"

namespace donner::experimental {
namespace {

constexpr float kDefaultWindowWidth = 420.0f;
constexpr float kDefaultWindowHeight = 520.0f;
constexpr double kMatrixDiffEpsilon = 1e-4;
constexpr float kCanvasHeight = 320.0f;
constexpr double kSnippetEpsilon = 1e-6;
constexpr const char* kDefaultTransform = "translate(30,20) rotate(30)";

const ImU32 kOriginalGeometryColor = IM_COL32(80, 140, 255, 255);
const ImU32 kTransformedGeometryColor = IM_COL32(255, 120, 80, 255);
const ImU32 kReferenceGeometryColor = IM_COL32(180, 120, 255, 255);
const ImU32 kOriginalBoundsColor = IM_COL32(80, 200, 140, 255);
const ImU32 kTransformedBoundsColor = IM_COL32(220, 80, 80, 255);
const ImU32 kReferenceBoundsColor = IM_COL32(140, 120, 220, 255);

}  // namespace

using donner::svg::PathSpline;
using donner::svg::parser::TransformParser;

void TransformInspector::State::rememberTransform() {
  if (transformString.empty()) {
    return;
  }

  auto existing = std::find(recentTransforms.begin(), recentTransforms.end(), transformString);
  if (existing != recentTransforms.end()) {
    recentTransforms.erase(existing);
  }

  recentTransforms.insert(recentTransforms.begin(), transformString);

  constexpr size_t kMaxEntries = 10;
  if (recentTransforms.size() > kMaxEntries) {
    recentTransforms.resize(kMaxEntries);
  }
}

std::optional<TransformInspector::DecomposedTransform> TransformInspector::decomposeTransform(
    const Transformd& transform) {
  const double a = transform.data[0];
  const double b = transform.data[1];
  const double c = transform.data[2];
  const double d = transform.data[3];
  const double tx = transform.data[4];
  const double ty = transform.data[5];

  const double scaleX = std::hypot(a, b);
  if (NearZero(scaleX)) {
    return std::nullopt;
  }

  const double rotationRadians = std::atan2(b, a);
  const double shear = (a * c + b * d) / (scaleX * scaleX);
  const double skewXRadians = std::atan(shear);

  const double adjustedC = c - a * shear;
  const double adjustedD = d - b * shear;
  double scaleY = std::hypot(adjustedC, adjustedD);
  const double determinant = a * d - b * c;
  if (determinant < 0.0) {
    scaleY = -scaleY;
  }

  DecomposedTransform decomposition;
  decomposition.translation = Vector2d(tx, ty);
  decomposition.scale = Vector2d(scaleX, scaleY);
  decomposition.rotationDegrees = rotationRadians * MathConstants<double>::kRadToDeg;
  decomposition.skewXDegrees = skewXRadians * MathConstants<double>::kRadToDeg;
  decomposition.skewYDegrees = 0.0;
  decomposition.valid = true;
  return decomposition;
}

std::string TransformInspector::serializeDecomposition(const DecomposedTransform& decomposition,
                                                       bool anglesInRadians) {
  std::ostringstream stream;
  stream.setf(std::ios::fixed);
  stream << std::setprecision(3);

  const double angleScale = anglesInRadians ? MathConstants<double>::kDegToRad : 1.0;
  const double rotationValue = decomposition.rotationDegrees * angleScale;
  const double skewXValue = decomposition.skewXDegrees * angleScale;
  const double skewYValue = decomposition.skewYDegrees * angleScale;

  stream << "translate(" << decomposition.translation.x << ", " << decomposition.translation.y
         << ")";
  stream << " rotate(" << rotationValue << ")";
  stream << " skewX(" << skewXValue << ")";
  stream << " skewY(" << skewYValue << ")";
  stream << " scale(" << decomposition.scale.x << ", " << decomposition.scale.y << ")";
  return stream.str();
}

std::string TransformInspector::escapeForSnippet(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (char ch : value) {
    if (ch == '"') {
      escaped += "\\\"";
    } else if (ch == '\\') {
      escaped += "\\\\";
    } else if (ch == '\n') {
      escaped += "\\n";
    } else {
      escaped.push_back(ch);
    }
  }
  return escaped;
}

std::vector<TransformInspector::Polyline> TransformInspector::buildRectangleGeometry(
    const Boxd& rect) {
  Polyline rectLine;
  rectLine.closed = true;
  rectLine.points = {{rect.topLeft.x, rect.topLeft.y},
                     {rect.bottomRight.x, rect.topLeft.y},
                     {rect.bottomRight.x, rect.bottomRight.y},
                     {rect.topLeft.x, rect.bottomRight.y}};
  return {rectLine};
}

std::vector<TransformInspector::Polyline> TransformInspector::samplePathGeometry(
    const PathSpline& path) {
  std::vector<Polyline> geometry;
  const auto& commands = path.commands();
  if (commands.empty()) {
    return geometry;
  }

  Polyline current;
  Vector2d subpathStart;
  auto flush = [&]() {
    if (!current.points.empty()) {
      geometry.push_back(current);
      current = Polyline();
    }
  };

  constexpr int kCurveSamples = 16;

  for (size_t i = 0; i < commands.size(); ++i) {
    const auto& command = commands[i];
    switch (command.type) {
      case PathSpline::CommandType::MoveTo: {
        flush();
        subpathStart = path.pointAt(i, 0.0);
        current.points.push_back(subpathStart);
        current.closed = false;
        break;
      }
      case PathSpline::CommandType::LineTo: {
        if (current.points.empty()) {
          current.points.push_back(path.pointAt(i, 0.0));
        }
        current.points.push_back(path.pointAt(i, 1.0));
        break;
      }
      case PathSpline::CommandType::CurveTo: {
        if (current.points.empty()) {
          current.points.push_back(path.pointAt(i, 0.0));
        }
        for (int step = 1; step <= kCurveSamples; ++step) {
          const double t = static_cast<double>(step) / kCurveSamples;
          current.points.push_back(path.pointAt(i, t));
        }
        break;
      }
      case PathSpline::CommandType::ClosePath: {
        if (!current.points.empty()) {
          current.points.push_back(subpathStart);
          current.closed = true;
          flush();
        }
        break;
      }
      default: break;
    }
  }

  flush();
  return geometry;
}

std::optional<Boxd> TransformInspector::computeBounds(const std::vector<Polyline>& geometry) {
  if (geometry.empty()) {
    return std::nullopt;
  }

  std::optional<Boxd> bounds;
  for (const Polyline& line : geometry) {
    for (const Vector2d& point : line.points) {
      if (!bounds) {
        bounds = Boxd::CreateEmpty(point);
      } else {
        bounds->addPoint(point);
      }
    }
  }
  return bounds;
}

std::vector<TransformInspector::Polyline> TransformInspector::applyTransform(
    const std::vector<Polyline>& geometry, const Transformd& transform) {
  std::vector<Polyline> transformed;
  transformed.reserve(geometry.size());
  for (const Polyline& line : geometry) {
    Polyline copy;
    copy.closed = line.closed;
    copy.points.reserve(line.points.size());
    for (const Vector2d& point : line.points) {
      copy.points.push_back(transform.transformPosition(point));
    }
    transformed.push_back(std::move(copy));
  }
  return transformed;
}

TransformInspector::Result TransformInspector::evaluate() const {
  Result result;

  TransformParser::Options options;
  options.angleUnit = state_.parserToggles.anglesInRadians ? TransformParser::AngleUnit::kRadians
                                                           : TransformParser::AngleUnit::kDegrees;

  auto maybeTransform = TransformParser::Parse(state_.transformString, options);
  if (maybeTransform.hasError()) {
    result.error = maybeTransform.error().reason.str();
  } else {
    result.parsed = true;
    result.transform = maybeTransform.result();
    if (auto maybeDecomposition = decomposeTransform(result.transform)) {
      result.decomposition = *maybeDecomposition;
    }
  }

  if (!state_.pathData.empty()) {
    const auto maybePath = PathParser::Parse(state_.pathData);
    if (maybePath.hasResult()) {
      result.geometry = samplePathGeometry(maybePath.result());
      result.geometryNote = "Using sampled path geometry for visualization.";
    }
    if (maybePath.hasError()) {
      result.geometryNote = maybePath.error().reason.str();
    }
  }

  if (result.geometry.empty()) {
    result.geometry = buildRectangleGeometry(state_.rect);
    if (result.geometryNote.empty()) {
      result.geometryNote = "Sampling rectangle inputs.";
    }
  }

  if (auto bounds = computeBounds(result.geometry)) {
    result.originalBounds = *bounds;
  } else {
    result.originalBounds = state_.rect;
  }

  result.transformedBounds = result.originalBounds;

  if (result.parsed) {
    result.transformedGeometry = applyTransform(result.geometry, result.transform);
    if (auto bounds = computeBounds(result.transformedGeometry)) {
      result.transformedBounds = *bounds;
    }
  }

  if (state_.referenceOptions.enabled) {
    TransformParser::Options referenceOptions;
    auto reference = TransformParser::Parse(state_.transformString, referenceOptions);
    if (reference.hasError()) {
      result.referenceError = reference.error().reason.str();
    } else {
      result.referenceParsed = true;
      result.referenceTransform = reference.result();
      result.referenceGeometry = applyTransform(result.geometry, result.referenceTransform);
      if (auto bounds = computeBounds(result.referenceGeometry)) {
        result.referenceBounds = *bounds;
      }
    }
  }
  return result;
}

std::string TransformInspector::buildTestSnippet(const Result& result) const {
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out << std::setprecision(6);

  out << "// destinationFromSource matrix and bounds for \""
      << escapeForSnippet(state_.transformString) << "\"\n";
  out << "TransformParser::Options options;\n";
  if (state_.parserToggles.anglesInRadians) {
    out << "options.angleUnit = TransformParser::AngleUnit::kRadians;\n";
  }
  out << "const auto parsed = TransformParser::Parse(\"" << escapeForSnippet(state_.transformString)
      << "\", options);\n";
  out << "ASSERT_TRUE(parsed.hasResult());\n";
  out << "const Transformd transform = parsed.result();\n";
  out << "EXPECT_NEAR(transform.data[0], " << result.transform.data[0] << ", " << kSnippetEpsilon
      << ");  // a\n";
  out << "EXPECT_NEAR(transform.data[1], " << result.transform.data[1] << ", " << kSnippetEpsilon
      << ");  // b\n";
  out << "EXPECT_NEAR(transform.data[2], " << result.transform.data[2] << ", " << kSnippetEpsilon
      << ");  // c\n";
  out << "EXPECT_NEAR(transform.data[3], " << result.transform.data[3] << ", " << kSnippetEpsilon
      << ");  // d\n";
  out << "EXPECT_NEAR(transform.data[4], " << result.transform.data[4] << ", " << kSnippetEpsilon
      << ");  // e\n";
  out << "EXPECT_NEAR(transform.data[5], " << result.transform.data[5] << ", " << kSnippetEpsilon
      << ");  // f\n";

  out << "\nconst Boxd original = Boxd::FromXYWH(" << result.originalBounds.topLeft.x << ", "
      << result.originalBounds.topLeft.y << ", " << result.originalBounds.width() << ", "
      << result.originalBounds.height() << ");\n";
  out << "const Boxd transformed = Boxd::FromXYWH(" << result.transformedBounds.topLeft.x << ", "
      << result.transformedBounds.topLeft.y << ", " << result.transformedBounds.width() << ", "
      << result.transformedBounds.height() << ");\n";
  out << "EXPECT_NEAR(transformed.topLeft.x, " << result.transformedBounds.topLeft.x << ", "
      << kSnippetEpsilon << ");\n";
  out << "EXPECT_NEAR(transformed.topLeft.y, " << result.transformedBounds.topLeft.y << ", "
      << kSnippetEpsilon << ");\n";
  out << "EXPECT_NEAR(transformed.width(), " << result.transformedBounds.width() << ", "
      << kSnippetEpsilon << ");\n";
  out << "EXPECT_NEAR(transformed.height(), " << result.transformedBounds.height() << ", "
      << kSnippetEpsilon << ");\n";

  if (!state_.pathData.empty()) {
    out << "// Path input was provided; geometry sampling drove bounds.\n";
  } else {
    out << "// Rectangle input: x=" << state_.rect.topLeft.x << ", y=" << state_.rect.topLeft.y
        << ", w=" << state_.rect.width() << ", h=" << state_.rect.height() << "\n";
  }

  return out.str();
}

void TransformInspector::applyGeneratedTransform(const std::string& transform) {
  state_.transformString = transform;
  state_.rememberTransform();
}

void TransformInspector::resetState() {
  state_.transformString = kDefaultTransform;
  state_.rect = Boxd::FromXYWH(0.0, 0.0, 120.0, 80.0);
  state_.pathData.clear();
  state_.recentTransforms.clear();
  state_.decomposition = DecomposedTransform();
  state_.parserToggles = ParserToggles();
  state_.referenceOptions = ReferenceOptions();
}

void TransformInspector::drawTransformHistory() {
  if (ImGui::Button("Save to history")) {
    state_.rememberTransform();
  }
  if (ImGui::BeginListBox("Recent transforms", ImVec2(-FLT_MIN, 110.0f))) {
    for (size_t i = 0; i < state_.recentTransforms.size(); ++i) {
      const std::string& item = state_.recentTransforms[i];
      const bool isSelected = (state_.transformString == item);
      if (ImGui::Selectable(item.c_str(), isSelected)) {
        state_.transformString = item;
      }
    }
    ImGui::EndListBox();
  }
}

void TransformInspector::drawRectangleInputs() {
  ImGui::Text("Rectangle");
  ImGui::DragScalar("X", ImGuiDataType_Double, &state_.rect.topLeft.x, 0.1f);
  ImGui::DragScalar("Y", ImGuiDataType_Double, &state_.rect.topLeft.y, 0.1f);
  double width = state_.rect.width();
  double height = state_.rect.height();
  if (ImGui::DragScalar("W", ImGuiDataType_Double, &width, 0.1f, 0.0f, DBL_MAX)) {
    state_.rect.bottomRight.x = state_.rect.topLeft.x + width;
  }
  if (ImGui::DragScalar("H", ImGuiDataType_Double, &height, 0.1f, 0.0f, DBL_MAX)) {
    state_.rect.bottomRight.y = state_.rect.topLeft.y + height;
  }
}

void TransformInspector::drawParserOptions() {
  if (ImGui::CollapsingHeader("Parser options", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::Checkbox("Angles are radians", &state_.parserToggles.anglesInRadians);
    ImGui::TextWrapped(
        "Toggle angle unit for rotation and skew parsing. Other parser options will be added in "
        "later steps with sensible defaults.");
  }
}

void TransformInspector::drawTransformActions() {
  if (ImGui::Button("Copy transform")) {
    ImGui::SetClipboardText(state_.transformString.c_str());
  }
  ImGui::SameLine();
  if (ImGui::Button("Reset inputs")) {
    resetState();
  }
  ImGui::SameLine();
  ImGui::TextDisabled("Clipboard + defaults");
}

void TransformInspector::drawReferenceOptions() {
  if (ImGui::CollapsingHeader("Reference compare", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::Checkbox("Enable reference matrix (degrees baseline)", &state_.referenceOptions.enabled);
    ImGui::TextWrapped(
        "Evaluates the same transform string using default degree-based parsing to compare against "
        "the current parser options. Highlights differences per matrix cell and bounds.");
  }
}

void TransformInspector::drawEdgeCaseHelpers() {
  if (ImGui::CollapsingHeader("Edge-case generators", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (ImGui::Button("Nested translate/rotate/scale")) {
      applyGeneratedTransform("translate(12 8) rotate(33) translate(-4 3) scale(1.2,-0.7)");
    }
    ImGui::SameLine();
    if (ImGui::Button("Scientific notation")) {
      applyGeneratedTransform("translate(1e-3,-2e2) rotate(1.57079632679) scale(0.5, -1.2)");
    }

    if (ImGui::Button("No separators")) {
      applyGeneratedTransform("translate(30 15)rotate(-15)skewX(12)scale(0.9 1.1)");
    }
    ImGui::SameLine();
    if (ImGui::Button("Randomized")) {
      std::mt19937 rng(static_cast<unsigned int>(ImGui::GetTime() * 1000.0f));
      std::uniform_real_distribution<double> angleDist(-180.0, 180.0);
      std::uniform_real_distribution<double> scaleDist(0.25, 1.75);
      std::uniform_real_distribution<double> translateDist(-120.0, 120.0);
      std::ostringstream stream;
      stream.setf(std::ios::fixed);
      stream << std::setprecision(3);
      stream << "translate(" << translateDist(rng) << ", " << translateDist(rng) << ") ";
      stream << "rotate(" << angleDist(rng) << ") ";
      stream << "skewX(" << angleDist(rng) << ") ";
      stream << "scale(" << scaleDist(rng) << ", " << scaleDist(rng) << ")";
      applyGeneratedTransform(stream.str());
    }

    ImGui::TextWrapped(
        "Use these presets to quickly exercise separator handling, exponentials, and nested"
        " transforms. Generated strings are saved to history for reuse.");
  }
}

void TransformInspector::drawDecomposition(Result& result) {
  ImGui::Text("Decomposed transform (edit to update string)");
  if (!result.parsed) {
    ImGui::TextWrapped("Enter a valid transform string to enable decomposition controls.");
    return;
  }
  if (!result.decomposition.valid) {
    ImGui::TextWrapped("Decomposition unavailable for this transform.");
    return;
  }

  state_.decomposition = result.decomposition;
  auto& decomposition = state_.decomposition;
  const double angleDisplayScale =
      state_.parserToggles.anglesInRadians ? MathConstants<double>::kDegToRad : 1.0;

  bool decompositionChanged = false;
  decompositionChanged |=
      ImGui::DragScalar("Translate X", ImGuiDataType_Double, &decomposition.translation.x, 0.1f);
  decompositionChanged |=
      ImGui::DragScalar("Translate Y", ImGuiDataType_Double, &decomposition.translation.y, 0.1f);

  double rotationDisplay = decomposition.rotationDegrees * angleDisplayScale;
  if (ImGui::DragScalar("Rotation", ImGuiDataType_Double, &rotationDisplay, 0.25f, nullptr, nullptr,
                        "%.3f")) {
    decomposition.rotationDegrees = rotationDisplay / angleDisplayScale;
    decompositionChanged = true;
  }

  double skewXDisplay = decomposition.skewXDegrees * angleDisplayScale;
  if (ImGui::DragScalar("Skew X", ImGuiDataType_Double, &skewXDisplay, 0.25f, nullptr, nullptr,
                        "%.3f")) {
    decomposition.skewXDegrees = skewXDisplay / angleDisplayScale;
    decompositionChanged = true;
  }

  double skewYDisplay = decomposition.skewYDegrees * angleDisplayScale;
  if (ImGui::DragScalar("Skew Y", ImGuiDataType_Double, &skewYDisplay, 0.25f, nullptr, nullptr,
                        "%.3f")) {
    decomposition.skewYDegrees = skewYDisplay / angleDisplayScale;
    decompositionChanged = true;
  }

  decompositionChanged |= ImGui::DragScalar("Scale X", ImGuiDataType_Double, &decomposition.scale.x,
                                            0.05f, nullptr, nullptr, "%.3f");
  decompositionChanged |= ImGui::DragScalar("Scale Y", ImGuiDataType_Double, &decomposition.scale.y,
                                            0.05f, nullptr, nullptr, "%.3f");

  if (ImGui::Button("Reset translation")) {
    decomposition.translation = Vector2d(0.0, 0.0);
    decompositionChanged = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Reset rotation/skew")) {
    decomposition.rotationDegrees = 0.0;
    decomposition.skewXDegrees = 0.0;
    decomposition.skewYDegrees = 0.0;
    decompositionChanged = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Reset scale")) {
    decomposition.scale = Vector2d(1.0, 1.0);
    decompositionChanged = true;
  }

  if (decompositionChanged) {
    state_.transformString =
        serializeDecomposition(decomposition, state_.parserToggles.anglesInRadians);
    result = evaluate();
  }
}

void TransformInspector::drawGeometryOverlay(const Result& result) {
  ImGui::Text("Geometry preview");
  if (!result.geometryNote.empty()) {
    ImGui::TextWrapped("%s", result.geometryNote.c_str());
  }

  const ImVec2 canvasSize(ImGui::GetContentRegionAvail().x, kCanvasHeight);
  const ImVec2 canvasPos = ImGui::GetCursorScreenPos();
  const ImVec2 canvasEnd(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y);
  ImDrawList* drawList = ImGui::GetWindowDrawList();
  drawList->AddRectFilled(canvasPos, canvasEnd, IM_COL32(20, 20, 20, 255));
  drawList->AddRect(canvasPos, canvasEnd, IM_COL32(70, 70, 70, 255));

  ImGui::InvisibleButton("GeometryCanvas", canvasSize);
  if (result.geometry.empty()) {
    ImGui::TextWrapped("No geometry to display yet.");
    return;
  }

  auto accumulateBounds = [](std::optional<Boxd> current, const std::optional<Boxd>& next) {
    if (!next) {
      return current;
    }
    if (!current) {
      return next;
    }
    current->addBox(*next);
    return current;
  };

  std::optional<Boxd> viewBounds = computeBounds(result.geometry);
  viewBounds = accumulateBounds(viewBounds, computeBounds(result.transformedGeometry));
  viewBounds = accumulateBounds(viewBounds, computeBounds(result.referenceGeometry));

  if (!viewBounds) {
    ImGui::TextWrapped("Unable to compute bounds for geometry preview.");
    return;
  }

  Boxd paddedBounds = *viewBounds;
  const double span = std::max(paddedBounds.width(), paddedBounds.height());
  const double padding = std::max(4.0, span * 0.1);
  paddedBounds.topLeft.x -= padding;
  paddedBounds.topLeft.y -= padding;
  paddedBounds.bottomRight.x += padding;
  paddedBounds.bottomRight.y += padding;

  const double width = paddedBounds.width();
  const double height = paddedBounds.height();
  const double scale = width > 0.0 && height > 0.0
                           ? std::min(canvasSize.x / static_cast<float>(width),
                                      canvasSize.y / static_cast<float>(height))
                           : 1.0;

  auto toScreen = [&](const Vector2d& point) {
    const float x = static_cast<float>((point.x - paddedBounds.topLeft.x) * scale) + canvasPos.x;
    const float y = static_cast<float>((point.y - paddedBounds.topLeft.y) * scale) + canvasPos.y;
    return ImVec2(x, y);
  };

  auto drawPolylines = [&](const std::vector<Polyline>& lines, ImU32 color) {
    for (const Polyline& line : lines) {
      if (line.points.size() < 2) {
        continue;
      }

      std::vector<ImVec2> polyline;
      polyline.reserve(line.points.size() + 1);
      for (const Vector2d& point : line.points) {
        polyline.push_back(toScreen(point));
      }
      if (line.closed) {
        polyline.push_back(polyline.front());
      }
      drawList->AddPolyline(polyline.data(), static_cast<int>(polyline.size()), color, false, 2.0f);
    }
  };

  auto drawBounds = [&](const Boxd& box, ImU32 color) {
    drawList->AddRect(toScreen(box.topLeft), toScreen(box.bottomRight), color, 0.0f, 0, 2.0f);
  };

  drawPolylines(result.geometry, kOriginalGeometryColor);
  drawBounds(result.originalBounds, kOriginalBoundsColor);

  if (!result.transformedGeometry.empty()) {
    drawPolylines(result.transformedGeometry, kTransformedGeometryColor);
    drawBounds(result.transformedBounds, kTransformedBoundsColor);
  }

  if (!result.referenceGeometry.empty()) {
    drawPolylines(result.referenceGeometry, kReferenceGeometryColor);
    drawBounds(result.referenceBounds, kReferenceBoundsColor);
  }

  const ImVec2 legendStart(canvasPos.x + 8.0f, canvasPos.y + 8.0f);
  const float legendLine = 10.0f;
  auto drawLegendEntry = [&](const char* label, ImU32 color, float offsetY) {
    const ImVec2 p1(legendStart.x, legendStart.y + offsetY);
    const ImVec2 p2(legendStart.x + legendLine, legendStart.y + offsetY);
    drawList->AddLine(p1, p2, color, 2.0f);
    drawList->AddText(ImVec2(p2.x + 6.0f, p2.y - 8.0f), IM_COL32_WHITE, label);
  };

  drawLegendEntry("Original geometry", kOriginalGeometryColor, 0.0f);
  drawLegendEntry("Transformed geometry", kTransformedGeometryColor, 16.0f);
  drawLegendEntry("Reference geometry", kReferenceGeometryColor, 32.0f);
  drawLegendEntry("Original bounds", kOriginalBoundsColor, 48.0f);
  drawLegendEntry("Transformed bounds", kTransformedBoundsColor, 64.0f);
  if (state_.referenceOptions.enabled) {
    drawLegendEntry("Reference bounds", kReferenceBoundsColor, 80.0f);
  }
}

void TransformInspector::drawParseResult(const Result& result) {
  ImGui::Text("Parse result");
  if (!result.error.empty()) {
    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Parse error: %s", result.error.c_str());
    return;
  }
  if (!result.parsed) {
    return;
  }

  auto renderMatrix = [](const char* label, const Transformd& transform) {
    ImGui::Text("%s", label);
    ImGui::Text("% .4f   % .4f   % .4f", transform.data[0], transform.data[2], transform.data[4]);
    ImGui::Text("% .4f   % .4f   % .4f", transform.data[1], transform.data[3], transform.data[5]);
  };

  ImGui::Text("Matrix (a c e / b d f)");
  renderMatrix("Donner", result.transform);

  if (state_.referenceOptions.enabled) {
    ImGui::Spacing();
    if (!result.referenceError.empty()) {
      ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Reference error: %s",
                         result.referenceError.c_str());
    } else if (result.referenceParsed) {
      renderMatrix("Reference", result.referenceTransform);

      if (ImGui::BeginTable("MatrixDiff", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Cell");
        ImGui::TableSetupColumn("Donner");
        ImGui::TableSetupColumn("Reference");
        ImGui::TableSetupColumn("Delta");
        ImGui::TableHeadersRow();

        const char* labels[] = {"a", "b", "c", "d", "e", "f"};
        const int indices[] = {0, 1, 2, 3, 4, 5};
        for (int i = 0; i < 6; ++i) {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::Text("%s", labels[i]);
          ImGui::TableSetColumnIndex(1);
          ImGui::Text("% .4f", result.transform.data[indices[i]]);
          ImGui::TableSetColumnIndex(2);
          ImGui::Text("% .4f", result.referenceTransform.data[indices[i]]);
          ImGui::TableSetColumnIndex(3);
          const double delta =
              result.transform.data[indices[i]] - result.referenceTransform.data[indices[i]];
          const bool highlight = std::abs(delta) > kMatrixDiffEpsilon;
          const ImVec4 color =
              highlight ? ImVec4(1.0f, 0.6f, 0.3f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
          ImGui::TextColored(color, "% .4f", delta);
        }
        ImGui::EndTable();
      }
    }
  }

  ImGui::Spacing();
  ImGui::Text("Bounds");
  ImGui::Text("Original: x=% .2f y=% .2f w=% .2f h=% .2f", result.originalBounds.topLeft.x,
              result.originalBounds.topLeft.y, result.originalBounds.width(),
              result.originalBounds.height());
  ImGui::Text("Transformed: x=% .2f y=% .2f w=% .2f h=% .2f", result.transformedBounds.topLeft.x,
              result.transformedBounds.topLeft.y, result.transformedBounds.width(),
              result.transformedBounds.height());

  if (state_.referenceOptions.enabled && result.referenceParsed) {
    ImGui::Text("Reference transformed: x=% .2f y=% .2f w=% .2f h=% .2f",
                result.referenceBounds.topLeft.x, result.referenceBounds.topLeft.y,
                result.referenceBounds.width(), result.referenceBounds.height());
  }

  ImGui::Spacing();
  ImGui::TextWrapped(
      "Geometry and bounds visuals reflect either the rectangle inputs or the sampled SVG path "
      "when provided.");
}

void TransformInspector::drawTestExport(const Result& result) {
  if (ImGui::CollapsingHeader("Test export", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (!result.parsed) {
      ImGui::TextWrapped("Provide a valid transform to enable export.");
      return;
    }

    const bool hasBounds =
        result.transformedBounds.width() > 0.0 || result.transformedBounds.height() > 0.0;
    if (hasBounds && ImGui::Button("Copy gtest snippet")) {
      const std::string snippet = buildTestSnippet(result);
      ImGui::SetClipboardText(snippet.c_str());
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Matrix + bounds in destinationFromSource notation.");

    if (!hasBounds) {
      ImGui::TextWrapped("Bounds unavailable for export; ensure geometry parsed correctly.");
    }
  }
}

void TransformInspector::drawHelpSection() {
  if (ImGui::CollapsingHeader("Help", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::TextWrapped(
        "Paste a transform string, tweak the rectangle or path input, and use the toggles below to "
        "exercise the parser. Geometry colors: blue = source outline, green = original bounds, red "
        "= parsed transform, orange = reference transform when enabled.");
    ImGui::BulletText("Edit the raw transform or use decomposition sliders; both stay in sync.");
    ImGui::BulletText(
        "Enable reference comparison to diff against a baseline parse that always uses degrees.");
    ImGui::BulletText(
        "Use edge-case generators to prefill tricky transform strings and store them in history.");
    ImGui::BulletText(
        "Copy the gtest snippet once parsing succeeds to seed golden tests in donner/svg/tests.");
  }
}

void TransformInspector::render() {
  if (!state_.isVisible) {
    return;
  }

  ImGui::SetNextWindowSize(ImVec2(kDefaultWindowWidth, kDefaultWindowHeight),
                           ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Transform Inspector", &state_.isVisible)) {
    drawHelpSection();
    ImGui::Separator();
    ImGui::InputTextMultiline("Transform", &state_.transformString, ImVec2(-FLT_MIN, 80.0f));
    drawTransformActions();
    drawTransformHistory();

    ImGui::Separator();
    drawRectangleInputs();

    ImGui::Separator();
    ImGui::Text("Optional path (overrides rectangle when non-empty)");
    ImGui::InputTextMultiline("Path d", &state_.pathData, ImVec2(-FLT_MIN, 80.0f));

    ImGui::Separator();
    drawParserOptions();
    drawReferenceOptions();
    drawEdgeCaseHelpers();

    Result parseResult = evaluate();

    ImGui::Separator();
    drawGeometryOverlay(parseResult);

    ImGui::Separator();
    drawDecomposition(parseResult);

    ImGui::Separator();
    drawTestExport(parseResult);

    ImGui::Separator();
    drawParseResult(parseResult);
  }
  ImGui::End();
}

}  // namespace donner::experimental
