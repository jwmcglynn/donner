#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

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

namespace donner::editor {
namespace {

constexpr std::string_view kStructuredStressSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="180" height="80" viewBox="0 0 180 80">
  <defs><filter id="blur"><feGaussianBlur in="SourceGraphic" stdDeviation="2"/></filter></defs>
  <rect id="plain" x="10" y="12" width="32" height="26" fill="red"/>
  <rect id="filtered" x="76" y="18" width="34" height="28" fill="blue" filter="url(#blur)" transform="translate(0 0)"/>
  <rect id="after" x="132" y="24" width="26" height="22" fill="green"/>
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

svg::RendererBitmap RenderReference(std::string_view source, const Vector2i& canvasSize) {
  EditorApp referenceApp;
  EXPECT_TRUE(referenceApp.loadFromString(source));
  referenceApp.document().document().setCanvasSize(canvasSize.x, canvasSize.y);

  svg::Renderer renderer;
  renderer.draw(referenceApp.document().document());
  return renderer.takeSnapshot();
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

  void ExpectInSync(std::string_view phase) {
    ASSERT_TRUE(app_.hasDocument()) << phase;
    EXPECT_EQ(textEditor_.getText(), app_.document().document().source()) << phase;
    EXPECT_FALSE(textEditor_.isTextChanged()) << phase;
    EXPECT_TRUE(app_.document().queue().empty()) << phase;
    EXPECT_EQ(app_.document().documentGeneration(), expectedDocumentGeneration_)
        << phase << ": localized structured edit replaced the SVG document";
    ExpectLiveSourceLocations(phase);

    svg::Renderer liveRenderer;
    liveRenderer.draw(app_.document().document());
    svg::RendererBitmap liveBitmap = liveRenderer.takeSnapshot();
    svg::RendererBitmap referenceBitmap =
        RenderReference(app_.document().document().source(), canvasSize_);
    tests::CompareBitmapToBitmap(liveBitmap, referenceBitmap, phase);

    RenderResult asyncResult = RenderAsyncPhase(phase);
    tests::CompareBitmapToBitmap(asyncResult.bitmap, referenceBitmap, phase);
  }

  void ExpectLiveSourceLocations(std::string_view phase) {
    ExpectElementSourceLocations(phase, "#plain", "plain");
    ExpectElementSourceLocations(phase, "#filtered", "filtered");
    ExpectElementSourceLocations(phase, "#after", "after");
  }

  void ExpectElementSourceLocations(std::string_view phase, std::string_view selector,
                                    std::string_view id) {
    std::optional<svg::SVGElement> element = app_.document().document().querySelector(selector);
    if (!element.has_value()) {
      return;
    }

    const std::string source(app_.document().document().source());
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
    ASSERT_FALSE(asyncRenderer_.isBusy()) << phase << ": click while async renderer is busy";
    selectTool_.onMouseDown(app_, documentPoint, MouseModifiers{});
    ASSERT_TRUE(app_.selectedElement().has_value()) << phase;
    EXPECT_EQ(app_.selectedElement()->id(), expectedId) << phase;
    selectTool_.onMouseUp(app_, documentPoint);
    ExpectInSync(phase);
  }

  void DragDocumentPoints(std::string_view phase, const Vector2d& startDocumentPoint,
                          const Vector2d& endDocumentPoint, std::string_view expectedId) {
    ASSERT_FALSE(asyncRenderer_.isBusy()) << phase << ": drag while async renderer is busy";
    selectTool_.onMouseDown(app_, startDocumentPoint, MouseModifiers{});
    ASSERT_TRUE(app_.selectedElement().has_value()) << phase;
    EXPECT_EQ(app_.selectedElement()->id(), expectedId) << phase;

    selectTool_.onMouseMove(app_, endDocumentPoint, /*buttonHeld=*/true);
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

  void ReplaceSourceText(std::string_view phase, std::string_view oldText,
                         std::string_view newText) {
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

  void ZoomAndPanAroundDocumentPoint(std::string_view phase, const Vector2d& documentPoint) {
    const Vector2d screenPoint = viewport_.documentToScreen(documentPoint);
    const Vector2d before = viewport_.screenToDocument(screenPoint);
    viewport_.zoomAround(viewport_.zoom * 1.7, screenPoint);
    viewport_.panBy(Vector2d(17.0, -9.0));
    EXPECT_NEAR(before.x, documentPoint.x, 1e-6) << phase;
    EXPECT_NEAR(before.y, documentPoint.y, 1e-6) << phase;
    ExpectInSync(phase);
  }

  void DeleteSelection(std::string_view phase) {
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

  RenderRequest BuildRenderRequest(std::uint64_t version) {
    RenderRequest request(asyncRendererBackend_, app_.document().document());
    request.version = version;
    request.documentGeneration = app_.document().documentGeneration();
    request.structuralRemap = app_.document().consumePendingStructuralRemap();
    request.selection = app_.selectedElement();
    if (app_.selectedElement().has_value() &&
        app_.selectedElement()->isa<svg::SVGGraphicsElement>()) {
      request.selectedEntity = app_.selectedElement()->entityHandle().entity();
    }
    if (std::optional<SelectTool::ActiveDragPreview> preview = selectTool_.activeDragPreview();
        preview.has_value()) {
      request.dragPreview = RenderRequest::DragPreview{
          .entity = preview->entity,
          .interactionKind = svg::compositor::InteractionHint::ActiveDrag,
      };
    }
    return request;
  }

  RenderResult RenderAsyncPhase(std::string_view phase) {
    asyncRenderer_.requestRender(BuildRenderRequest(++asyncVersion_));

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
};

TEST_F(StructuredEditingStressTest, MixedGuiTextZoomDeleteAndFilteredMoveStayInSync) {
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

}  // namespace
}  // namespace donner::editor
