/// @file
/// Generator for the v0.8 Donner showcase asset.
///
/// The showcase is authored as a GUI workflow ("open the base
/// splash in Donner Editor → add SVG text → Convert Text to Outlines → Export
/// Viewport as SVG with overlay"). The editor GUI cannot run headlessly in CI,
/// so this tool produces the final asset PROGRAMMATICALLY by driving the SAME
/// merged code paths the editor uses:
///
///   1. Load the editable base splash (`donner_splash_v0_8_editable.svg`).
///   2. Splice a `<text id="showcase_svg_label">SVG</text>` into the source,
///      rooted inside the root `<svg>` (mirrors the Text tool placing a `<text>`
///      inside the document root).
///   3. Run `convertTextToOutlines(...)` - the exact helper backing the editor's
///      `ConvertTextToOutlines` command - to replace the live `<text>` with an
///      outline `<g id="showcase_svg_label_outlines"
///      data-donner-converted-from="text">` of `<path>` glyphs.
///   4. Run `ExportViewportAsSvg(...)` with `includeSelectionOverlay = true` and
///      a selection snapshot for the outline group, framing the whole splash, so
///      the exported asset carries the `id="donner-editor-overlay"` chrome around
///      the outlined `SVG` letters (the "overlay enabled" export variant).
///   5. Write the export to the output path.
///
/// Run via:
///   bazel run //donner/editor/tools:generate_showcase_asset -- \
///       $PWD/donner_splash_v0_8_editable.svg $PWD/donner_splash_v0_8.svg
///
/// Keeping this a runnable tool (rather than a one-off manual GUI session) keeps
/// the showcase reproducible for future releases.

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/editor/OverlayRenderer.h"
#include "donner/editor/TextToOutlines.h"
#include "donner/editor/ViewportState.h"
#include "donner/editor/ViewportSvgExport.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::editor {
namespace {

/// Stable id for the inserted showcase text and, by the conversion's naming
/// convention (`<id>_outlines`), the resulting outline group.
constexpr std::string_view kLabelId = "showcase_svg_label";
constexpr std::string_view kOutlineGroupId = "showcase_svg_label_outlines";

/// Read an entire file into a string. Returns std::nullopt on open failure.
std::string ReadFile(const std::string& path, bool& ok) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream.is_open()) {
    ok = false;
    return {};
  }
  std::ostringstream buf;
  buf << stream.rdbuf();
  ok = true;
  return buf.str();
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

/// Splice the showcase `<text>SVG</text>` into \p source immediately before the
/// final `</svg>` so it is rooted inside the document root, exactly where the
/// Text tool inserts authored text. Coordinates sit in the lower third of the
/// 892x512 splash viewBox, with a large sans-serif font and a contrasting fill.
std::string InsertShowcaseText(std::string_view source, const Box2d& viewBox) {
  const double x = viewBox.topLeft.x + viewBox.width() * 0.34;
  const double y = viewBox.topLeft.y + viewBox.height() * 0.78;

  std::ostringstream textEl;
  textEl << "<text id=\"" << kLabelId << "\" x=\"" << x << "\" y=\"" << y
         << "\" font-family=\"sans-serif\" font-size=\"96\" fill=\"#0b1f4d\">SVG</text>\n";

  const std::string needle = "</svg>";
  std::string out(source);
  const size_t pos = out.rfind(needle);
  if (pos == std::string::npos) {
    // No close tag found; append (parser is lenient, but this should not happen
    // for the well-formed editable splash).
    out += textEl.str();
    return out;
  }
  out.insert(pos, textEl.str());
  return out;
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

int Run(const std::string& inputPath, const std::string& outputPath) {
  // 1. Load the editable base splash.
  bool readOk = false;
  const std::string editableSource = ReadFile(inputPath, readOk);
  if (!readOk) {
    std::cerr << "error: could not open input " << inputPath << "\n";
    return 1;
  }

  svg::SVGDocument baseDocument;
  std::string error;
  if (!ParseDocument(editableSource, baseDocument, error)) {
    std::cerr << "error: failed to parse input " << inputPath << ": " << error << "\n";
    return 1;
  }
  const std::optional<Box2d> viewBox = baseDocument.svgElement().viewBox();
  if (!viewBox.has_value() || viewBox->width() <= 0.0 || viewBox->height() <= 0.0) {
    std::cerr << "error: input " << inputPath << " has no usable root viewBox\n";
    return 1;
  }

  // 2. Insert the showcase `<text>` element (the Text tool's insertion path).
  const std::string withText = InsertShowcaseText(editableSource, *viewBox);
  svg::SVGDocument textDocument;
  if (!ParseDocument(withText, textDocument, error)) {
    std::cerr << "error: failed to parse text-augmented document: " << error << "\n";
    return 1;
  }
  std::optional<svg::SVGElement> textElement =
      textDocument.querySelector("#" + std::string(kLabelId));
  if (!textElement.has_value()) {
    std::cerr << "error: inserted <text> element not found after parse\n";
    return 1;
  }

  // 3. Convert Text to Outlines (the editor's ConvertTextToOutlines code
  //    path): build the detached outline group, then apply it as structural
  //    DOM edits mirroring the shell - insert the group before the <text>,
  //    insert its paths, delete the <text>.
  ConvertTextToOutlinesResult outlines = convertTextToOutlines(textDocument, *textElement);
  if (!outlines.ok) {
    std::cerr << "error: convertTextToOutlines failed: " << outlines.error << "\n";
    return 1;
  }
  std::optional<svg::SVGElement> textParent = textElement->parentElement();
  if (!textParent.has_value()) {
    std::cerr << "error: inserted <text> element has no parent\n";
    return 1;
  }
  (void)textDocument.insertElement(*textParent, *outlines.outlineGroup, *textElement);
  for (svg::SVGElement& path : outlines.outlinePaths) {
    (void)textDocument.insertElement(*outlines.outlineGroup, path);
  }
  (void)textDocument.removeElement(*textElement);

  svg::SVGDocument& outlinedDocument = textDocument;
  std::optional<svg::SVGElement> outlineGroup =
      outlinedDocument.querySelector("#" + std::string(kOutlineGroupId));
  if (!outlineGroup.has_value()) {
    std::cerr << "error: outline group #" << kOutlineGroupId << " not found after conversion\n";
    return 1;
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
    std::cerr << "error: ExportViewportAsSvg failed: " << exported.error << "\n";
    return 1;
  }

  // 5. Write the final asset.
  std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    std::cerr << "error: could not open output " << outputPath << " for writing\n";
    return 1;
  }
  out << exported.value;
  out.close();
  if (!out) {
    std::cerr << "error: failed while writing output " << outputPath << "\n";
    return 1;
  }

  std::cout << "Wrote showcase asset: " << outputPath << " (" << exported.value.size()
            << " bytes)\n";
  return 0;
}

}  // namespace
}  // namespace donner::editor

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: " << (argc > 0 ? argv[0] : "generate_showcase_asset")
              << " <input_editable.svg> <output_final.svg>\n";
    return 2;
  }
  return donner::editor::Run(argv[1], argv[2]);
}
