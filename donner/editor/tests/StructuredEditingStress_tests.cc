#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/RcString.h"
#include "donner/base/Transform.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/editor/AsyncRenderer.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/DocumentSyncController.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/TextEditor.h"
#include "donner/editor/ViewportState.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererImageIO.h"

namespace donner::editor {
namespace {

constexpr std::string_view kStructuredStressSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="180" height="80" viewBox="0 0 180 80">
  <defs>
    <filter id="blur"><feGaussianBlur in="SourceGraphic" stdDeviation="2"/></filter>
    <clipPath id="clip"><rect x="116" y="50" width="44" height="24"/></clipPath>
  </defs>
  <rect id="background" width="180" height="80" fill="white"/>
  <rect id="plain" x="10" y="12" width="32" height="26" fill="red"/>
  <rect id="filtered" x="76" y="18" width="34" height="28" fill="blue" filter="url(#blur)" transform="translate(0 0)"/>
  <rect id="after" x="132" y="24" width="26" height="22" fill="green"/>
  <path id="pathShape" d="M 20 56 L 42 56 L 30 72 Z" fill="black"/>
  <rect id="styled" x="70" y="54" width="34" height="18" style="fill: teal"/>
  <g id="clippedOpacity" clip-path="url(#clip)" opacity="0.86" transform="translate(0 0)">
    <rect x="118" y="50" width="34" height="24" fill="purple"/>
  </g>
</svg>)svg";

Coordinates CoordinatesForOffset(std::string_view text, std::size_t offset) {
  Coordinates result(0, 0);
  for (std::size_t i = 0; i < offset && i < text.size(); ++i) {
    if (text[i] == '\n') {
      ++result.line;
      result.column = 0;
    } else {
      ++result.column;
    }
  }
  return result;
}

std::string SourceSlice(std::string_view source, const SourceRange& range) {
  if (!range.start.offset.has_value() || !range.end.offset.has_value()) {
    return {};
  }

  const std::size_t start = range.start.offset.value();
  const std::size_t end = range.end.offset.value();
  if (start > end || end > source.size()) {
    return {};
  }

  return std::string(source.substr(start, end - start));
}

std::filesystem::path ArtifactOutputDir() {
  if (const char* outputDir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR")) {
    return outputDir;
  }

  return std::filesystem::temp_directory_path();
}

std::string SanitizeArtifactName(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (char ch : value) {
    const bool keep = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
    result.push_back(keep ? ch : '_');
  }

  return result.empty() ? "checkpoint" : result;
}

void WriteTextArtifact(const std::filesystem::path& path, std::string_view contents) {
  std::ofstream out(path);
  out << contents;
}

void WriteBitmapArtifact(const std::filesystem::path& path, const svg::RendererBitmap& bitmap) {
  if (bitmap.empty()) {
    return;
  }

  svg::RendererImageIO::writeRgbaPixelsToPngFile(path.string().c_str(), bitmap.pixels,
                                                 bitmap.dimensions.x, bitmap.dimensions.y,
                                                 bitmap.rowBytes);
}

svg::RendererBitmap BuildDiffBitmap(const svg::RendererBitmap& actual,
                                    const svg::RendererBitmap& expected) {
  if (actual.empty() || expected.empty()) {
    return {};
  }

  svg::RendererBitmap diff;
  diff.dimensions = Vector2i(std::max(actual.dimensions.x, expected.dimensions.x),
                             std::max(actual.dimensions.y, expected.dimensions.y));
  diff.rowBytes = static_cast<std::size_t>(diff.dimensions.x) * 4u;
  diff.alphaType = svg::AlphaType::Unpremultiplied;
  diff.pixels.assign(diff.rowBytes * static_cast<std::size_t>(diff.dimensions.y), 255u);

  for (int y = 0; y < diff.dimensions.y; ++y) {
    for (int x = 0; x < diff.dimensions.x; ++x) {
      const bool hasActual = x < actual.dimensions.x && y < actual.dimensions.y;
      const bool hasExpected = x < expected.dimensions.x && y < expected.dimensions.y;
      bool differs = hasActual != hasExpected;
      if (hasActual && hasExpected) {
        const std::size_t actualOffset =
            static_cast<std::size_t>(y) * actual.rowBytes + static_cast<std::size_t>(x) * 4u;
        const std::size_t expectedOffset =
            static_cast<std::size_t>(y) * expected.rowBytes + static_cast<std::size_t>(x) * 4u;
        for (std::size_t channel = 0; channel < 4u; ++channel) {
          differs |=
              actual.pixels[actualOffset + channel] != expected.pixels[expectedOffset + channel];
        }
      }

      if (differs) {
        const std::size_t diffOffset =
            static_cast<std::size_t>(y) * diff.rowBytes + static_cast<std::size_t>(x) * 4u;
        diff.pixels[diffOffset] = 255u;
        diff.pixels[diffOffset + 1u] = 0u;
        diff.pixels[diffOffset + 2u] = 0u;
        diff.pixels[diffOffset + 3u] = 255u;
      }
    }
  }

  return diff;
}

svg::RendererBitmap BuildSideBySideBitmap(const svg::RendererBitmap& expected,
                                          const svg::RendererBitmap& actual) {
  if (actual.empty() || expected.empty()) {
    return {};
  }

  svg::RendererBitmap sideBySide;
  sideBySide.dimensions = Vector2i(expected.dimensions.x + actual.dimensions.x,
                                   std::max(expected.dimensions.y, actual.dimensions.y));
  sideBySide.rowBytes = static_cast<std::size_t>(sideBySide.dimensions.x) * 4u;
  sideBySide.alphaType = actual.alphaType;
  sideBySide.pixels.assign(sideBySide.rowBytes * static_cast<std::size_t>(sideBySide.dimensions.y),
                           0u);

  for (int y = 0; y < expected.dimensions.y; ++y) {
    const std::size_t sourceOffset = static_cast<std::size_t>(y) * expected.rowBytes;
    const std::size_t targetOffset = static_cast<std::size_t>(y) * sideBySide.rowBytes;
    std::copy_n(expected.pixels.data() + sourceOffset,
                static_cast<std::size_t>(expected.dimensions.x) * 4u,
                sideBySide.pixels.data() + targetOffset);
  }

  for (int y = 0; y < actual.dimensions.y; ++y) {
    const std::size_t sourceOffset = static_cast<std::size_t>(y) * actual.rowBytes;
    const std::size_t targetOffset = static_cast<std::size_t>(y) * sideBySide.rowBytes +
                                     static_cast<std::size_t>(expected.dimensions.x) * 4u;
    std::copy_n(actual.pixels.data() + sourceOffset,
                static_cast<std::size_t>(actual.dimensions.x) * 4u,
                sideBySide.pixels.data() + targetOffset);
  }

  return sideBySide;
}

svg::RendererBitmap RenderReference(std::string_view source, const Vector2i& canvasSize) {
  EditorApp referenceApp;
  EXPECT_TRUE(referenceApp.loadFromString(source));
  referenceApp.document().document().setCanvasSize(canvasSize.x, canvasSize.y);

  svg::Renderer renderer;
  renderer.draw(referenceApp.document().document());
  return renderer.takeSnapshot();
}

std::uint32_t NextXorShift(std::uint32_t& state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

double RandomBetween(std::uint32_t& state, double min, double max) {
  const double unit = static_cast<double>(NextXorShift(state) % 1000u) / 999.0;
  return min + (max - min) * unit;
}

class StructuredEditingStressTest : public ::testing::Test {
protected:
  void SetUp() override {
    IMGUI_CHECKVERSION();
    imguiContext_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800, 600);
    io.Fonts->Build();

    app_.setStructuredEditingEnabled(true);
    ASSERT_TRUE(app_.loadFromString(kStructuredStressSvg));
    app_.document().document().setCanvasSize(canvasSize_.x, canvasSize_.y);
    app_.setCleanSourceText(kStructuredStressSvg);
    expectedDocumentGeneration_ = app_.document().documentGeneration();

    textEditor_.setText(kStructuredStressSvg);
    controller_ = std::make_optional<DocumentSyncController>(std::string(kStructuredStressSvg));
    controller_->handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);

    viewport_.paneOrigin = Vector2d(0.0, 0.0);
    viewport_.paneSize = Vector2d(360.0, 160.0);
    viewport_.documentViewBox = Box2d(Vector2d(0.0, 0.0), Vector2d(180.0, 80.0));
    viewport_.resetTo100Percent();
  }

  void TearDown() override {
    if (imguiContext_ != nullptr) {
      ImGui::DestroyContext(imguiContext_);
      imguiContext_ = nullptr;
    }
  }

  class FailureArtifactScope {
  public:
    FailureArtifactScope(StructuredEditingStressTest& owner, std::string_view phase)
        : owner_(owner), phase_(phase), hadFailureAtStart_(::testing::Test::HasFailure()) {}

    ~FailureArtifactScope() {
      if (!hadFailureAtStart_ && ::testing::Test::HasFailure()) {
        owner_.WriteFailureArtifacts(phase_);
      }
    }

  private:
    StructuredEditingStressTest& owner_;
    std::string phase_;
    bool hadFailureAtStart_ = false;
  };

  void RecordAction(std::string_view phase) { actionLog_.emplace_back(phase); }

  void WriteFailureArtifacts(std::string_view phase) {
    if (failureArtifactsWritten_) {
      return;
    }
    failureArtifactsWritten_ = true;
    WriteFailureArtifactsToDirectory(ArtifactOutputDir(), phase);
  }

  void WriteFailureArtifactsToDirectory(const std::filesystem::path& outputDir,
                                        std::string_view phase) {
    std::filesystem::create_directories(outputDir);
    const std::string label = SanitizeArtifactName(phase);

    std::string source;
    if (app_.hasDocument()) {
      source = std::string(app_.document().document().source());
      WriteTextArtifact(outputDir / ("structured_editing_" + label + "_source.svg"), source);
    }
    WriteTextArtifact(outputDir / ("structured_editing_" + label + "_editor_text.svg"),
                      textEditor_.getText());
    WriteTextArtifact(outputDir / ("structured_editing_" + label + "_manifest.txt"),
                      BuildFailureManifest(phase));

    std::ostringstream actions;
    for (std::size_t i = 0; i < actionLog_.size(); ++i) {
      actions << i << ": " << actionLog_[i] << '\n';
    }
    WriteTextArtifact(outputDir / ("structured_editing_" + label + "_actions.txt"), actions.str());

    if (app_.hasDocument()) {
      svg::Renderer liveRenderer;
      liveRenderer.draw(app_.document().document());
      svg::RendererBitmap liveBitmap = liveRenderer.takeSnapshot();
      WriteBitmapArtifact(outputDir / ("structured_editing_" + label + "_live.png"), liveBitmap);

      if (!source.empty()) {
        EditorApp referenceApp;
        if (referenceApp.loadFromString(source)) {
          referenceApp.document().document().setCanvasSize(canvasSize_.x, canvasSize_.y);
          svg::Renderer referenceRenderer;
          referenceRenderer.draw(referenceApp.document().document());
          svg::RendererBitmap referenceBitmap = referenceRenderer.takeSnapshot();
          WriteBitmapComparisonArtifacts(outputDir, "structured_editing_" + label, liveBitmap,
                                         referenceBitmap);
        }
      }
    }

    if (lastSettledBitmap_.has_value() && lastSettledReferenceBitmap_.has_value()) {
      WriteBitmapComparisonArtifacts(outputDir, "structured_editing_" + label + "_last_settled",
                                     *lastSettledBitmap_, *lastSettledReferenceBitmap_);
    }
  }

  std::string BuildFailureManifest(std::string_view phase) {
    std::ostringstream manifest;
    manifest << "phase: " << phase << '\n';
    manifest << "last_settled_phase: " << lastSettledPhase_ << '\n';
    manifest << "has_document: " << app_.hasDocument() << '\n';
    manifest << "text_changed: " << textEditor_.isTextChanged() << '\n';
    manifest << "editor_text_size: " << textEditor_.getText().size() << '\n';
    manifest << "expected_document_generation: " << expectedDocumentGeneration_ << '\n';
    manifest << "async_busy: " << asyncRenderer_.isBusy() << '\n';
    manifest << "viewport_zoom: " << viewport_.zoom << '\n';
    manifest << "viewport_pan_doc_point: " << viewport_.panDocPoint.x << ","
             << viewport_.panDocPoint.y << '\n';
    manifest << "viewport_pan_screen_point: " << viewport_.panScreenPoint.x << ","
             << viewport_.panScreenPoint.y << '\n';

    if (!app_.hasDocument()) {
      return manifest.str();
    }

    manifest << "document_generation: " << app_.document().documentGeneration() << '\n';
    manifest << "frame_version: " << app_.document().currentFrameVersion() << '\n';
    manifest << "document_source_size: " << app_.document().document().source().size() << '\n';
    manifest << "command_queue_size: " << app_.document().queue().size() << '\n';
    if (const auto& parseError = app_.document().lastParseError(); parseError.has_value()) {
      manifest << "parse_error: " << parseError->severity << ": " << parseError->reason << '\n';
      manifest << "parse_error_range: ";
      if (parseError->range.start.offset.has_value()) {
        manifest << parseError->range.start.offset.value();
      } else {
        manifest << "unknown";
      }
      manifest << "..";
      if (parseError->range.end.offset.has_value()) {
        manifest << parseError->range.end.offset.value();
      } else {
        manifest << "unknown";
      }
      manifest << '\n';
    } else {
      manifest << "parse_error: none\n";
    }

    const std::vector<svg::SVGElement> selected = app_.selectedElements();
    manifest << "selected_count: " << selected.size() << '\n';
    for (std::size_t i = 0; i < selected.size(); ++i) {
      manifest << "selected_" << i << ": " << selected[i].id() << '\n';
    }

    return manifest.str();
  }

  void WriteBitmapComparisonArtifacts(const std::filesystem::path& outputDir,
                                      const std::string& artifactBase,
                                      const svg::RendererBitmap& liveBitmap,
                                      const svg::RendererBitmap& referenceBitmap) {
    WriteBitmapArtifact(outputDir / (artifactBase + "_live.png"), liveBitmap);
    WriteBitmapArtifact(outputDir / (artifactBase + "_reference.png"), referenceBitmap);
    WriteBitmapArtifact(outputDir / (artifactBase + "_diff.png"),
                        BuildDiffBitmap(liveBitmap, referenceBitmap));
    WriteBitmapArtifact(outputDir / (artifactBase + "_side_by_side.png"),
                        BuildSideBySideBitmap(referenceBitmap, liveBitmap));
  }

  void StoreLastSettledBitmaps(std::string_view phase, const svg::RendererBitmap& liveBitmap,
                               const svg::RendererBitmap& referenceBitmap) {
    lastSettledPhase_ = std::string(phase);
    lastSettledBitmap_ = liveBitmap;
    lastSettledReferenceBitmap_ = referenceBitmap;
  }

  void ExpectInSync(std::string_view phase) {
    FailureArtifactScope artifacts(*this, phase);
    const bool hadFailureAtStart = ::testing::Test::HasFailure();

    ASSERT_TRUE(app_.hasDocument()) << phase;
    EXPECT_EQ(textEditor_.getText(), app_.document().document().source()) << phase;
    EXPECT_FALSE(textEditor_.isTextChanged()) << phase;
    EXPECT_TRUE(app_.document().queue().empty()) << phase;
    EXPECT_EQ(app_.document().documentGeneration(), expectedDocumentGeneration_)
        << phase << ": localized structured edit replaced the SVG document";
    EXPECT_FALSE(app_.document().lastParseError().has_value()) << phase;
    ExpectLiveSourceLocations(phase);

    svg::RendererBitmap referenceBitmap =
        RenderReference(app_.document().document().source(), canvasSize_);
    RenderResult asyncResult = RenderAsyncPhase(phase);
    tests::CompareBitmapToBitmap(asyncResult.bitmap, referenceBitmap, phase);

    svg::Renderer liveRenderer;
    liveRenderer.draw(app_.document().document());
    svg::RendererBitmap liveBitmap = liveRenderer.takeSnapshot();
    tests::CompareBitmapToBitmap(liveBitmap, referenceBitmap, phase);
    if (::testing::Test::HasFailure() == hadFailureAtStart) {
      StoreLastSettledBitmaps(phase, liveBitmap, referenceBitmap);
    }
  }

  svg::RendererBitmap RenderLiveBitmap() {
    svg::Renderer liveRenderer;
    liveRenderer.draw(app_.document().document());
    return liveRenderer.takeSnapshot();
  }

  void ReplaceSourceTextExpectDiagnostic(std::string_view phase, std::string_view oldText,
                                         std::string_view newText) {
    RecordAction(phase);
    FailureArtifactScope artifacts(*this, phase);
    const svg::RendererBitmap lastGoodBitmap = RenderLiveBitmap();
    const std::string currentText = textEditor_.getText();
    const std::size_t offset = currentText.find(oldText);
    ASSERT_NE(offset, std::string::npos) << phase;

    textEditor_.setSelection(CoordinatesForOffset(currentText, offset),
                             CoordinatesForOffset(currentText, offset + oldText.size()));
    textEditor_.insertText(newText);
    controller_->handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);
    controller_->handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.2f);

    ASSERT_TRUE(app_.hasDocument()) << phase;
    EXPECT_EQ(textEditor_.getText(), app_.document().document().source()) << phase;
    EXPECT_FALSE(textEditor_.isTextChanged()) << phase;
    EXPECT_TRUE(app_.document().queue().empty()) << phase;
    EXPECT_EQ(app_.document().documentGeneration(), expectedDocumentGeneration_)
        << phase << ": diagnostic path replaced the SVG document";
    ASSERT_TRUE(app_.document().lastParseError().has_value()) << phase;

    tests::CompareBitmapToBitmap(RenderLiveBitmap(), lastGoodBitmap, phase);
    RenderResult asyncResult = RenderAsyncPhase(phase);
    tests::CompareBitmapToBitmap(asyncResult.bitmap, lastGoodBitmap, phase);
  }

  void ExpectLiveSourceLocations(std::string_view phase) {
    ExpectElementSourceLocations(phase, "#plain", "plain");
    ExpectElementSourceLocations(phase, "#filtered", "filtered");
    ExpectElementSourceLocations(phase, "#after", "after");
    ExpectElementSourceLocations(phase, "#pathShape", "pathShape");
    ExpectElementSourceLocations(phase, "#styled", "styled");
    ExpectElementSourceLocations(phase, "#clippedOpacity", "clippedOpacity");
  }

  void ExpectElementSourceLocations(std::string_view phase, std::string_view selector,
                                    std::string_view id) {
    std::optional<svg::SVGElement> element = app_.document().document().querySelector(selector);
    if (!element.has_value()) {
      return;
    }

    const std::string source(app_.document().document().source());
    // §concurrent-dom: hold a scoped read access for the XML/source inspection below — the call
    // chain (XMLNode::TryCast → entity resolution → getNodeLocation) hits guarded EntityHandle
    // accessors that assert under ConcurrentDom without a scoped access guard.
    [[maybe_unused]] svg::DocumentReadAccess access = app_.document().document().readAccess();
    std::optional<xml::XMLNode> xmlNode = xml::XMLNode::TryCast(element->entityHandle());
    ASSERT_TRUE(xmlNode.has_value()) << phase << ": " << id << " is not backed by XML";

    std::optional<SourceRange> nodeLocation = xmlNode->getNodeLocation();
    ASSERT_TRUE(nodeLocation.has_value()) << phase << ": " << id << " has no node source range";
    const std::string nodeSource = SourceSlice(source, *nodeLocation);
    ASSERT_FALSE(nodeSource.empty()) << phase << ": " << id << " node range resolved empty";
    EXPECT_NE(nodeSource.find(std::string("id=\"") + std::string(id) + "\""), std::string::npos)
        << phase << ": " << id << " node source range points at wrong bytes: " << nodeSource;

    ExpectAttributeSourceLocation(phase, *xmlNode, *element, "id");
    ExpectAttributeSourceLocation(phase, *xmlNode, *element, "fill");
    ExpectAttributeSourceLocation(phase, *xmlNode, *element, "filter");
    ExpectAttributeSourceLocation(phase, *xmlNode, *element, "transform");
    ExpectAttributeSourceLocation(phase, *xmlNode, *element, "clip-path");
    ExpectAttributeSourceLocation(phase, *xmlNode, *element, "opacity");
    ExpectAttributeSourceLocation(phase, *xmlNode, *element, "d");
    ExpectAttributeSourceLocation(phase, *xmlNode, *element, "style");
    ExpectAttributeSourceLocation(phase, *xmlNode, *element, "stroke");
  }

  void ExpectAttributeSourceLocation(std::string_view phase, const xml::XMLNode& xmlNode,
                                     const svg::SVGElement& element, std::string_view attrName) {
    if (!element.hasAttribute(attrName)) {
      return;
    }

    const std::string source(app_.document().document().source());
    std::optional<SourceRange> attributeLocation =
        xmlNode.getAttributeLocation(source, xml::XMLQualifiedNameRef(attrName));
    ASSERT_TRUE(attributeLocation.has_value())
        << phase << ": missing source range for " << element.id() << " @" << attrName;

    const std::string attributeSource = SourceSlice(source, *attributeLocation);
    ASSERT_FALSE(attributeSource.empty())
        << phase << ": empty source range for " << element.id() << " @" << attrName;
    EXPECT_NE(attributeSource.find(attrName), std::string::npos)
        << phase << ": attribute range points at wrong name: " << attributeSource;

    std::optional<RcString> value = element.getAttribute(attrName);
    ASSERT_TRUE(value.has_value())
        << phase << ": missing live value for " << element.id() << " @" << attrName;
    EXPECT_NE(attributeSource.find(std::string(*value)), std::string::npos)
        << phase << ": attribute range points at stale value: " << attributeSource;
  }

  void ClickDocumentPoint(std::string_view phase, const Vector2d& documentPoint,
                          std::string_view expectedId) {
    RecordAction(phase);
    ASSERT_FALSE(asyncRenderer_.isBusy()) << phase << ": click while async renderer is busy";
    selectTool_.onMouseDown(app_, documentPoint, ViewportMouseModifiers());
    ASSERT_TRUE(app_.selectedElement().has_value()) << phase;
    EXPECT_EQ(app_.selectedElement()->id(), expectedId) << phase;
    selectTool_.onMouseUp(app_, documentPoint);
    ExpectInSync(phase);
  }

  void DragDocumentPoints(std::string_view phase, const Vector2d& startDocumentPoint,
                          const Vector2d& endDocumentPoint, std::string_view expectedId) {
    RecordAction(phase);
    ASSERT_FALSE(asyncRenderer_.isBusy()) << phase << ": drag while async renderer is busy";
    const MouseModifiers modifiers = ViewportMouseModifiers();
    selectTool_.onMouseDown(app_, startDocumentPoint, modifiers);
    ASSERT_TRUE(app_.selectedElement().has_value()) << phase;
    EXPECT_EQ(app_.selectedElement()->id(), expectedId) << phase;

    selectTool_.onMouseMove(app_, endDocumentPoint, /*buttonHeld=*/true, modifiers);
    ASSERT_TRUE(app_.flushFrame()) << phase;
    selectTool_.onMouseUp(app_, endDocumentPoint);
    controller_->applyPendingWritebacks(app_, selectTool_, textEditor_);
    ExpectInSync(phase);
  }

  void DragScreenPoints(std::string_view phase, const Vector2d& startScreenPoint,
                        const Vector2d& endScreenPoint, std::string_view expectedId) {
    DragDocumentPoints(phase, viewport_.screenToDocument(startScreenPoint),
                       viewport_.screenToDocument(endScreenPoint), expectedId);
  }

  MouseModifiers ViewportMouseModifiers() const {
    MouseModifiers modifiers;
    modifiers.pixelsPerDocUnit = viewport_.pixelsPerDocUnit();
    return modifiers;
  }

  void ReplaceSourceText(std::string_view phase, std::string_view oldText,
                         std::string_view newText) {
    RecordAction(phase);
    const std::string currentText = textEditor_.getText();
    const std::size_t offset = currentText.find(oldText);
    ASSERT_NE(offset, std::string::npos) << phase;

    textEditor_.setSelection(CoordinatesForOffset(currentText, offset),
                             CoordinatesForOffset(currentText, offset + oldText.size()));
    textEditor_.insertText(newText);
    controller_->handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);
    controller_->handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.2f);
    ExpectInSync(phase);
  }

  void DeleteSourceElement(std::string_view phase, std::string_view id) {
    const std::string currentText = textEditor_.getText();
    const std::string idNeedle = "id=\"" + std::string(id) + "\"";
    const std::size_t idOffset = currentText.find(idNeedle);
    ASSERT_NE(idOffset, std::string::npos) << phase;

    const std::size_t tagStart = currentText.rfind('<', idOffset);
    ASSERT_NE(tagStart, std::string::npos) << phase;
    const std::size_t tagEnd = currentText.find("/>", idOffset);
    ASSERT_NE(tagEnd, std::string::npos) << phase;

    ReplaceSourceText(phase, std::string_view(currentText).substr(tagStart, tagEnd + 2u - tagStart),
                      "");
  }

  void ZoomAndPanAroundDocumentPoint(std::string_view phase, const Vector2d& documentPoint) {
    RecordAction(phase);
    const Vector2d screenPoint = viewport_.documentToScreen(documentPoint);
    const Vector2d before = viewport_.screenToDocument(screenPoint);
    viewport_.zoomAround(viewport_.zoom * 1.7, screenPoint);
    viewport_.panBy(Vector2d(17.0, -9.0));
    EXPECT_NEAR(before.x, documentPoint.x, 1e-6) << phase;
    EXPECT_NEAR(before.y, documentPoint.y, 1e-6) << phase;
    ExpectInSync(phase);
  }

  void DeleteSelection(std::string_view phase) {
    RecordAction(phase);
    ASSERT_FALSE(asyncRenderer_.isBusy()) << phase << ": delete while async renderer is busy";
    const std::vector<svg::SVGElement> selected = app_.selectedElements();
    ASSERT_FALSE(selected.empty()) << phase;

    app_.setSelection(std::nullopt);
    for (const svg::SVGElement& element : selected) {
      if (std::optional<AttributeWritebackTarget> target = captureAttributeWritebackTarget(element);
          target.has_value()) {
        app_.enqueueElementRemoveWriteback(EditorApp::CompletedElementRemoveWriteback{
            .target = *target,
        });
      }
      app_.applyMutation(EditorCommand::DeleteElementCommand(element));
    }

    ASSERT_TRUE(app_.flushFrame()) << phase;
    EXPECT_EQ(app_.document().lastFlushResult().sourceDeltas.size(), selected.size())
        << phase << ": delete did not emit one XML source delta per removed element";
    controller_->applyPendingWritebacks(app_, selectTool_, textEditor_);
    ExpectInSync(phase);
  }

  void DeleteSelectionAfterStartingRender(std::string_view phase) {
    RecordAction(phase);
    ASSERT_FALSE(asyncRenderer_.isBusy()) << phase << ": render already busy before delete test";
    const std::vector<svg::SVGElement> selected = app_.selectedElements();
    ASSERT_FALSE(selected.empty()) << phase;

    struct PendingDelete {
      svg::SVGElement element;
      std::optional<AttributeWritebackTarget> target;
    };
    std::vector<PendingDelete> pendingDeletes;
    pendingDeletes.reserve(selected.size());
    for (const svg::SVGElement& element : selected) {
      pendingDeletes.push_back(PendingDelete{
          .element = element,
          .target = captureAttributeWritebackTarget(element),
      });
    }

    asyncRenderer_.requestRender(BuildRenderRequest(++asyncVersion_));
    ASSERT_TRUE(asyncRenderer_.isBusy()) << phase << ": render request did not enter busy state";

    app_.setSelection(std::nullopt);
    for (const PendingDelete& pendingDelete : pendingDeletes) {
      if (pendingDelete.target.has_value()) {
        app_.enqueueElementRemoveWriteback(EditorApp::CompletedElementRemoveWriteback{
            .target = *pendingDelete.target,
        });
      }
      app_.applyMutation(EditorCommand::DeleteElementCommand(pendingDelete.element));
    }
    EXPECT_FALSE(app_.document().queue().empty()) << phase;

    RenderResult busyResult = WaitForAsyncFinalPhase(phase);
    ASSERT_FALSE(asyncRenderer_.isBusy()) << phase << ": async renderer stayed busy after poll";

    ASSERT_TRUE(app_.flushFrame()) << phase;
    EXPECT_EQ(app_.document().lastFlushResult().sourceDeltas.size(), selected.size())
        << phase << ": delete did not emit one XML source delta per removed element";
    controller_->applyPendingWritebacks(app_, selectTool_, textEditor_);
    ExpectInSync(phase);
  }

  RenderRequest BuildRenderRequest(std::uint64_t version, bool includeInteractionState = true) {
    RenderRequest request(asyncRendererBackend_, app_.document().document());
    request.version = version;
    request.documentGeneration = app_.document().documentGeneration();
    request.structuralRemap = app_.document().consumePendingStructuralRemap();
    if (!includeInteractionState) {
      return request;
    }

    request.selection = app_.selectedElement();
    if (app_.selectedElement().has_value() &&
        app_.selectedElement()->isa<svg::SVGGraphicsElement>()) {
      request.selectedEntity = app_.selectedElement()->unsafeEntityHandle().entity();
    }
    if (std::optional<SelectTool::ActiveDragPreview> preview = selectTool_.activeDragPreview();
        preview.has_value()) {
      request.dragPreview = RenderRequest::DragPreview{
          .entity = preview->entity,
          .interactionKind = svg::compositor::InteractionHint::ActiveDrag,
          .translation = preview->translation,
          .documentFromCachedDocument = preview->documentFromCachedDocument,
          .dragGeneration = preview->dragGeneration,
      };
    }
    return request;
  }

  RenderResult RenderAsyncPhase(std::string_view phase) {
    asyncRenderer_.requestRender(
        BuildRenderRequest(++asyncVersion_, /*includeInteractionState=*/false));
    return WaitForAsyncFinalPhase(phase);
  }

  RenderResult WaitForAsyncFinalPhase(std::string_view phase) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      std::optional<RenderResult> result = asyncRenderer_.pollResult();
      if (result.has_value()) {
        EXPECT_FALSE(asyncRenderer_.isBusy()) << phase << ": async renderer stayed busy";
        return std::move(*result);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ADD_FAILURE() << phase << ": async render timed out";
    EXPECT_FALSE(asyncRenderer_.isBusy()) << phase << ": async renderer stayed busy after timeout";
    return RenderResult();
  }

  ImGuiContext* imguiContext_ = nullptr;
  EditorApp app_;
  TextEditor textEditor_;
  std::optional<DocumentSyncController> controller_;
  SelectTool selectTool_;
  AsyncRenderer asyncRenderer_;
  svg::Renderer asyncRendererBackend_;
  ViewportState viewport_;
  Vector2i canvasSize_ = Vector2i(180, 80);
  std::uint64_t expectedDocumentGeneration_ = 0;
  std::uint64_t asyncVersion_ = 0;
  std::vector<std::string> actionLog_;
  std::optional<svg::RendererBitmap> lastSettledBitmap_;
  std::optional<svg::RendererBitmap> lastSettledReferenceBitmap_;
  std::string lastSettledPhase_;
  bool failureArtifactsWritten_ = false;
};

TEST_F(StructuredEditingStressTest, MixedGuiTextZoomDeleteAndFilteredMoveStayInSync) {
  FailureArtifactScope artifacts(*this, "mixed scenario");

  ExpectInSync("initial");

  DragScreenPoints("plain drag after zoom-ready click", viewport_.documentToScreen({20.0, 20.0}),
                   viewport_.documentToScreen({32.0, 24.0}), "plain");
  ZoomAndPanAroundDocumentPoint("zoom and pan after plain drag", Vector2d(32.0, 24.0));

  ReplaceSourceText("source fill edit", R"(fill="red")", R"(fill="orange")");
  ReplaceSourceText("source filtered transform edit", "transform=\"translate(0 0)\"",
                    "transform=\"translate(14 0)\"");

  std::optional<svg::SVGElement> filtered = app_.document().document().querySelector("#filtered");
  ASSERT_TRUE(filtered.has_value());
  const Transform2d sourceEditedTransform = filtered->cast<svg::SVGGraphicsElement>().transform();
  EXPECT_DOUBLE_EQ(sourceEditedTransform.data[4], 14.0);

  DragScreenPoints("filtered drag after source move", viewport_.documentToScreen({104.0, 32.0}),
                   viewport_.documentToScreen({116.0, 38.0}), "filtered");
  DeleteSelection("delete filtered after mixed edits");

  EXPECT_FALSE(app_.document().document().querySelector("#filtered").has_value());
  ClickDocumentPoint("click after delete still selects", Vector2d(140.0, 32.0), "after");
}

TEST_F(StructuredEditingStressTest, CanvasToSourceScenariosStayInSync) {
  FailureArtifactScope artifacts(*this, "canvas to source scenarios");

  ExpectInSync("canvas initial");

  const std::string beforePlainDrag = textEditor_.getText();
  DragScreenPoints("canvas drag plain", viewport_.documentToScreen({20.0, 20.0}),
                   viewport_.documentToScreen({34.0, 27.0}), "plain");
  EXPECT_NE(textEditor_.getText(), beforePlainDrag);
  ASSERT_TRUE(app_.document().document().querySelector("#plain").has_value());
  EXPECT_TRUE(app_.document().document().querySelector("#plain")->hasAttribute("transform"));

  ZoomAndPanAroundDocumentPoint("canvas repeated zoom one", Vector2d(92.0, 32.0));
  DragScreenPoints("canvas drag filtered after zoom", viewport_.documentToScreen({94.0, 32.0}),
                   viewport_.documentToScreen({108.0, 40.0}), "filtered");
  ZoomAndPanAroundDocumentPoint("canvas repeated zoom two", Vector2d(134.0, 62.0));
  DragScreenPoints("canvas drag clipped opacity after zoom",
                   viewport_.documentToScreen({134.0, 62.0}),
                   viewport_.documentToScreen({146.0, 66.0}), "clippedOpacity");

  DeleteSelectionAfterStartingRender("canvas delete clipped while render busy");
  EXPECT_FALSE(app_.document().document().querySelector("#clippedOpacity").has_value());
  ClickDocumentPoint("canvas post-delete click selects after", Vector2d(140.0, 34.0), "after");
}

TEST_F(StructuredEditingStressTest, SourceToCanvasScenariosStayInSync) {
  FailureArtifactScope artifacts(*this, "source to canvas scenarios");

  ExpectInSync("source initial");

  ReplaceSourceText("source fill value edit", R"(fill="red")", R"(fill="orange")");
  ReplaceSourceText("source transform value edit", "transform=\"translate(0 0)\"",
                    "transform=\"translate(8 0)\"");
  ReplaceSourceText("source path d edit", R"(d="M 20 56 L 42 56 L 30 72 Z")",
                    R"(d="M 18 54 L 46 60 L 30 74 Z")");
  ReplaceSourceText("source inline style edit", R"(style="fill: teal")", R"(style="fill: orange")");
  ReplaceSourceText("source insert path stroke attribute", R"(id="pathShape" )",
                    R"(id="pathShape" stroke="white" )");

  ClickDocumentPoint("source edited path remains selectable", Vector2d(30.0, 64.0), "pathShape");
  ReplaceSourceTextExpectDiagnostic("source malformed style quote", R"(style="fill: orange")",
                                    R"(style="fill: orange)");
  ReplaceSourceText("source restore style quote", R"(style="fill: orange)",
                    R"(style="fill: orange")");
}

TEST_F(StructuredEditingStressTest, SourceDeleteClearsStaleHitTargets) {
  FailureArtifactScope artifacts(*this, "source delete stale hit targets");

  ExpectInSync("source delete initial");
  DeleteSourceElement("source delete styled element", "styled");

  EXPECT_FALSE(app_.document().document().querySelector("#styled").has_value());
  ClickDocumentPoint("source deleted element old location selects background", Vector2d(84.0, 62.0),
                     "background");
  ClickDocumentPoint("source delete nearby click still selects clipped", Vector2d(134.0, 62.0),
                     "clippedOpacity");
}

// TODO(#601): Re-enable once the multi-thread determinism test framework lands. This stress test
// drives the async render worker (ExpectInSync / RecordAction) and then renders reference/live
// snapshots in WriteFailureArtifactsToDirectory, so the document is transiently
// ThreadingMode::ConcurrentDom while the UI thread reads it. The unguarded read race is caught as
// an `ElementAnchor::assertScopedEntityHandleAccessAllowed` abort in asserts-on builds but becomes
// a use-after-free segfault when the assert is compiled out (observed on CI's NDEBUG linux runner;
// passes locally because the race is timing/load dependent). Tracked by the determinism-framework
// task (#601).
TEST_F(StructuredEditingStressTest, DISABLED_FailureArtifactsIncludeReplayAndBitmapDiffFiles) {
  ExpectInSync("artifact smoke baseline");
  RecordAction("artifact smoke action");

  const std::string phase = "artifact smoke";
  const std::string label = SanitizeArtifactName(phase);
  const auto uniqueId = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path outputDir =
      std::filesystem::temp_directory_path() /
      ("structured_editing_artifact_test_" + std::to_string(uniqueId));

  WriteFailureArtifactsToDirectory(outputDir, phase);

  const auto expectArtifact = [&](std::string_view suffix) {
    const std::filesystem::path path =
        outputDir / ("structured_editing_" + label + std::string(suffix));
    EXPECT_TRUE(std::filesystem::exists(path)) << path;
  };

  expectArtifact("_source.svg");
  expectArtifact("_editor_text.svg");
  expectArtifact("_manifest.txt");
  expectArtifact("_actions.txt");
  expectArtifact("_live.png");
  expectArtifact("_reference.png");
  expectArtifact("_diff.png");
  expectArtifact("_side_by_side.png");
  expectArtifact("_last_settled_live.png");
  expectArtifact("_last_settled_reference.png");
  expectArtifact("_last_settled_diff.png");
  expectArtifact("_last_settled_side_by_side.png");

  if (!::testing::Test::HasFailure()) {
    std::filesystem::remove_all(outputDir);
  }
}

TEST_F(StructuredEditingStressTest, SeededRandomizedMixedActionsStayInSync) {
  constexpr std::uint32_t kSeed = 0x5EED575u;
  std::uint32_t rng = kSeed;
  FailureArtifactScope artifacts(*this, "seeded randomized mixed actions");
  RecordAction("seed=" + std::to_string(kSeed));

  ExpectInSync("seeded initial");
  ClickDocumentPoint("seeded click plain", Vector2d(22.0, 24.0), "plain");

  ZoomAndPanAroundDocumentPoint("seeded zoom and pan", Vector2d(RandomBetween(rng, 18.0, 34.0),
                                                                RandomBetween(rng, 18.0, 32.0)));

  const Vector2d plainStart(24.0, 24.0);
  const Vector2d plainEnd(plainStart.x + RandomBetween(rng, 8.0, 14.0),
                          plainStart.y + RandomBetween(rng, 3.0, 8.0));
  DragScreenPoints("seeded drag plain", viewport_.documentToScreen(plainStart),
                   viewport_.documentToScreen(plainEnd), "plain");

  ReplaceSourceText("seeded fill source edit", R"(fill="red")", R"(fill="purple")");
  ReplaceSourceTextExpectDiagnostic("seeded malformed quote", R"(fill="purple")",
                                    R"(fill="purple)");
  ReplaceSourceText("seeded restore quote", R"(fill="purple)", R"(fill="purple")");

  const double sourceMoveX = RandomBetween(rng, 10.0, 18.0);
  ReplaceSourceText("seeded filtered source transform", "transform=\"translate(0 0)\"",
                    "transform=\"translate(" + std::to_string(sourceMoveX) + " 0)\"");

  const Vector2d filteredStart(96.0 + sourceMoveX, 32.0);
  const Vector2d filteredEnd(filteredStart.x + RandomBetween(rng, 7.0, 13.0),
                             filteredStart.y + RandomBetween(rng, 4.0, 9.0));
  DragScreenPoints("seeded drag filtered", viewport_.documentToScreen(filteredStart),
                   viewport_.documentToScreen(filteredEnd), "filtered");

  DeleteSelection("seeded delete filtered");
  ClickDocumentPoint("seeded post-delete click", Vector2d(144.0, 34.0), "after");
}

}  // namespace
}  // namespace donner::editor
