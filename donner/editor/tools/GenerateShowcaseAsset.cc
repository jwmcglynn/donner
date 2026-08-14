/// @file
/// In-memory generator for the v0.8 Donner showcase asset.
///
/// The showcase is authored as a GUI workflow ("open the base
/// splash in Donner Editor → add SVG text → Convert Text to Outlines → Export
/// Viewport as SVG with overlay"). The editor GUI cannot run headlessly in CI,
/// so this tool produces the derived output programmatically by driving the same
/// merged code paths the editor uses:
///
///   1. Load the canonical base splash (`donner_splash.svg`).
///   2. Create and insert a `<text id="showcase_svg_label">SVG</text>` through
///      the SVG DOM and structured source-edit APIs used by the Text tool.
///   3. Run `convertTextToOutlines(...)` - the exact helper backing the editor's
///      `ConvertTextToOutlines` command - to replace the live `<text>` with an
///      outline `<g id="showcase_svg_label_outlines"
///      data-donner-converted-from="text">` of `<path>` glyphs.
///   4. Run `ExportViewportAsSvg(...)` with `includeSelectionOverlay = true` and
///      a selection snapshot for the outline group, framing the whole splash, so
///      the exported asset carries the `id="donner-editor-overlay"` chrome around
///      the outlined `SVG` letters (the "overlay enabled" export variant).
///   5. Return the export to the caller.
///
/// Run via:
///   bazel run //donner/editor/tools:generate_showcase_asset -- \
///       $PWD/donner_splash.svg /tmp/donner-showcase.svg
///
/// Keeping the generator in memory lets the demo and tests create the derived
/// showcase on demand without storing generated splash variants in the repo.

#include "donner/editor/tools/GenerateShowcaseAsset.h"

#include <cmath>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/StringUtils.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/editor/OverlayRenderer.h"
#include "donner/editor/TextToOutlines.h"
#include "donner/editor/ViewportState.h"
#include "donner/editor/ViewportSvgExport.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/SVGTextElement.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::editor {
namespace {

/// Stable id for the inserted showcase text and, by the conversion's naming
/// convention (`<id>_outlines`), the resulting outline group.
constexpr std::string_view kLabelId = "showcase_svg_label";
constexpr std::string_view kOutlineGroupId = "showcase_svg_label_outlines";
constexpr std::string_view kOverlayGroupId = "donner-editor-overlay";

/// Find the first id that the generator or its downstream conversion/export
/// steps reserve. `convertTextToOutlines` derives path ids by suffixing the
/// outline-group id, so the entire label-id prefix is reserved rather than only
/// the two ids created directly here.
std::optional<std::string> FindReservedShowcaseId(const svg::SVGElement& root) {
  std::vector<svg::SVGElement> pending{root};
  while (!pending.empty()) {
    const svg::SVGElement element = std::move(pending.back());
    pending.pop_back();

    if (const auto id = element.getAttribute("id"); id.has_value()) {
      const std::string value = id->str();
      if (value.starts_with(kLabelId) || value == kOverlayGroupId) {
        return value;
      }
    }

    for (std::optional<svg::SVGElement> child = element.firstChild(); child.has_value();
         child = child->nextSibling()) {
      pending.push_back(*child);
    }
  }

  return std::nullopt;
}

/// Parse \p source into a source-backed document, or fail loudly. Source backing
/// is required so `convertTextToOutlines` can locate the `<text>` source range.
bool ParseDocument(std::string_view source, svg::SVGDocument& outDocument, std::string& error) {
  ParseWarningSink sink = ParseWarningSink::Disabled();
  auto result = svg::parser::SVGParser::ParseSVG(source, sink);
  if (result.hasError()) {
    std::ostringstream os;
    os << result.error();
    error = os.str();
    return false;
  }
  outDocument = std::move(result).result();
  return true;
}

/// Return a diagnostic when a structured source edit did not apply.
std::optional<std::string> ValidateSourceEdit(std::string_view action,
                                              const xml::ApplySourceEditResult& result) {
  if (result.diagnostic.has_value()) {
    std::ostringstream os;
    os << action << " failed: " << result.diagnostic->reason;
    return os.str();
  }
  if (!result.applied) {
    return std::string(action) + " did not update the source-backed document";
  }
  return std::nullopt;
}

/// Insert the showcase `<text>SVG</text>` through the same DOM-first mutation
/// seam as editor-authored text. Coordinates place a compact white badge to the
/// right of the Donner wordmark without obscuring the existing lettering.
std::optional<svg::SVGTextElement> InsertShowcaseText(svg::SVGDocument& document,
                                                      const Box2d& viewBox, std::string& error) {
  const double x = viewBox.topLeft.x + viewBox.width() * 0.77;
  const double y = viewBox.topLeft.y + viewBox.height() * 0.72;

  svg::SVGTextElement text = svg::SVGTextElement::Create(document);
  text.setAttribute("id", kLabelId);
  text.setAttribute("x", donner::detail::FormatNumberForSVG(x));
  text.setAttribute("y", donner::detail::FormatNumberForSVG(y));
  text.setAttribute("font-family", "sans-serif");
  text.setAttribute("font-size", "60");
  text.setAttribute("font-weight", "bold");
  text.setAttribute("style", "fill: #ffffff");

  const xml::ApplySourceEditResult inserted = document.insertElement(document.svgElement(), text);
  if (const std::optional<std::string> editError =
          ValidateSourceEdit("inserting showcase text", inserted)) {
    error = *editError;
    return std::nullopt;
  }

  const xml::ApplySourceEditResult content = document.setElementTextContent(text, "SVG");
  if (const std::optional<std::string> editError =
          ValidateSourceEdit("setting showcase text content", content)) {
    error = *editError;
    return std::nullopt;
  }
  text.setTextContent("SVG");
  return text;
}

/// Build a viewport that frames the entire splash: the document viewBox maps 1:1
/// to a render pane the size of the splash's pixel dimensions. Mirrors the
/// `MakeViewport` helper in `ViewportSvgExport_tests.cc`.
ViewportState MakeFullDocumentViewport(const Box2d& documentViewBox) {
  ViewportState viewport;
  viewport.paneOrigin = Vector2d::Zero();
  viewport.paneSize = Vector2d(documentViewBox.width(), documentViewBox.height());
  viewport.devicePixelRatio = 1.0;
  viewport.documentViewBox = documentViewBox;
  viewport.panDocPoint = documentViewBox.topLeft;
  viewport.panScreenPoint = Vector2d::Zero();
  viewport.zoom = 1.0;
  viewport.resetTo100Percent();
  return viewport;
}

}  // namespace

Result<std::string, std::string> GenerateShowcaseAsset(std::string_view source) {
  // 1. Parse the canonical base splash supplied by the caller.
  svg::SVGDocument textDocument;
  std::string error;
  if (!ParseDocument(source, textDocument, error)) {
    return Result<std::string, std::string>::Err("failed to parse input: " + error);
  }
  const std::optional<Box2d> viewBox = textDocument.svgElement().viewBox();
  if (!viewBox.has_value()) {
    return Result<std::string, std::string>::Err("input has no usable root viewBox");
  }
  const double viewBoxWidth = viewBox->width();
  const double viewBoxHeight = viewBox->height();
  if (!std::isfinite(viewBox->topLeft.x) || !std::isfinite(viewBox->topLeft.y) ||
      !std::isfinite(viewBox->bottomRight.x) || !std::isfinite(viewBox->bottomRight.y) ||
      !std::isfinite(viewBoxWidth) || !std::isfinite(viewBoxHeight)) {
    return Result<std::string, std::string>::Err(
        "input root viewBox must contain only finite coordinates and dimensions");
  }
  if (viewBoxWidth <= 0.0 || viewBoxHeight <= 0.0) {
    return Result<std::string, std::string>::Err("input has no usable root viewBox");
  }
  constexpr double kMaxViewportDimension = static_cast<double>(std::numeric_limits<int>::max());
  if (viewBoxWidth > kMaxViewportDimension || viewBoxHeight > kMaxViewportDimension) {
    return Result<std::string, std::string>::Err(
        "input root viewBox exceeds the supported viewport dimension range");
  }

  if (const std::optional<std::string> reservedId =
          FindReservedShowcaseId(textDocument.svgElement())) {
    return Result<std::string, std::string>::Err("input already uses reserved id #" + *reservedId);
  }
  if (textDocument.querySelector("text").has_value()) {
    return Result<std::string, std::string>::Err(
        "input must not contain live <text> elements; use the canonical splash");
  }

  // 2. Insert the showcase `<text>` through the Text tool's DOM-first seam.
  std::optional<svg::SVGTextElement> insertedText =
      InsertShowcaseText(textDocument, *viewBox, error);
  if (!insertedText.has_value()) {
    return Result<std::string, std::string>::Err(std::move(error));
  }
  svg::SVGTextElement textElement = std::move(*insertedText);

  // 3. Convert Text to Outlines (the editor's ConvertTextToOutlines code
  //    path): build the detached outline group, then apply it as structural
  //    DOM edits mirroring the shell - insert the group before the <text>,
  //    insert its paths, delete the <text>.
  ConvertTextToOutlinesResult outlines = convertTextToOutlines(textDocument, textElement);
  if (!outlines.ok) {
    return Result<std::string, std::string>::Err("convertTextToOutlines failed: " + outlines.error);
  }
  if (outlines.outlinePaths.empty()) {
    return Result<std::string, std::string>::Err("convertTextToOutlines produced no outline paths");
  }
  std::optional<svg::SVGElement> textParent = textElement.parentElement();
  if (!textParent.has_value()) {
    return Result<std::string, std::string>::Err("inserted <text> element has no parent");
  }
  const xml::ApplySourceEditResult insertedGroup =
      textDocument.insertElement(*textParent, *outlines.outlineGroup, textElement);
  if (const std::optional<std::string> editError =
          ValidateSourceEdit("inserting outline group", insertedGroup)) {
    return Result<std::string, std::string>::Err(*editError);
  }
  for (svg::SVGElement& path : outlines.outlinePaths) {
    const xml::ApplySourceEditResult insertedPath =
        textDocument.insertElement(*outlines.outlineGroup, path);
    if (const std::optional<std::string> editError =
            ValidateSourceEdit("inserting outline path", insertedPath)) {
      return Result<std::string, std::string>::Err(*editError);
    }
  }
  const xml::ApplySourceEditResult removedText = textDocument.removeElement(textElement);
  if (const std::optional<std::string> editError =
          ValidateSourceEdit("removing source text", removedText)) {
    return Result<std::string, std::string>::Err(*editError);
  }

  svg::SVGDocument& outlinedDocument = textDocument;
  std::optional<svg::SVGElement> outlineGroup =
      outlinedDocument.querySelector("#" + std::string(kOutlineGroupId));
  if (!outlineGroup.has_value()) {
    return Result<std::string, std::string>::Err("outline group #" + std::string(kOutlineGroupId) +
                                                 " not found after conversion");
  }
  if (!outlinedDocument.querySelector("#" + std::string(kOutlineGroupId) + "_0").has_value()) {
    return Result<std::string, std::string>::Err(
        "first generated outline path is missing after conversion");
  }

  // 4. Export the viewport as SVG with the outline group selected, so the
  //    overlay chrome (id="donner-editor-overlay") wraps the SVG letters. The
  //    overlay snapshot is captured with an identity canvasFromDoc because the
  //    export serializes overlay primitives in document space (same convention
  //    as ViewportSvgExport_tests::MakeOverlaySnapshot).
  std::vector<svg::SVGElement> selection{*outlineGroup};
  const SelectionChromeSnapshot snapshot = OverlayRenderer::captureChromeSnapshot(
      selection, /*marqueeRectDoc=*/std::nullopt, Transform2d());

  const ViewportState viewport = MakeFullDocumentViewport(*viewBox);
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(static_cast<int>(viewBox->width()),
                                                      static_cast<int>(viewBox->height())));

  ViewportExportOptions options;
  options.transparentBackground = true;
  options.includeSelectionOverlay = true;

  const auto exported =
      ExportViewportAsSvg(outlinedDocument, viewport, renderPaneRect, options, &snapshot);
  if (!exported.ok()) {
    return Result<std::string, std::string>::Err("ExportViewportAsSvg failed: " + exported.error);
  }

  // 5. Return the generated asset. Callers choose whether to display it,
  // inspect it in memory, or write it to a temporary path.
  return Result<std::string, std::string>::Ok(std::move(exported.value));
}

}  // namespace donner::editor
