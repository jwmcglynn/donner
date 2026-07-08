/// @file ViewportSvgExportRoundTrip_fuzzer.cc
///
/// Headless libFuzzer target for the editor's viewport SVG export/serialization
/// path (\ref donner::editor::ExportViewportAsSvg in ViewportSvgExport.cc).
///
/// This is the editor's export/serialize surface: it hand-writes XML as
/// strings (attribute carry-over, an injected `<defs><clipPath>`, a wrapping
/// `<g>`) rather than going through a structured XML writer, which is exactly
/// the kind of code that can mishandle escaping of untrusted attribute/text
/// content. The fuzzer:
///
///   1. Parses the raw input bytes as SVG via the engine's untrusted-input
///      parser (SVGParser::ParseSVG). Malformed input is expected and simply
///      rejected - this is never asserted on.
///   2. On a successful parse with an owned XML source store, runs the
///      viewport exporter (\ref ExportViewportAsSvg) against a fixed,
///      deterministic viewport/render-pane rect derived from a couple of
///      fuzzer-controlled bits (transparent background / selection overlay
///      toggles), so the fuzzer still explores both code paths without
///      spending entropy on geometry.
///   3. When the export succeeds, re-parses the exporter's own output.
///      This is the stronger oracle for the exporter: **the serializer must
///      never emit output that the engine's own parser rejects.** A crash or
///      a second-parse failure here indicates a serializer correctness bug
///      (unescaped/mis-escaped attribute or text content, malformed injected
///      markup, etc.) which is a real security concern for an exporter that
///      round-trips untrusted content.
///
/// Export refusals (e.g. external `href`/`xlink:href` references) are
/// expected and not asserted on.

#include <fuzzer/FuzzedDataProvider.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/ParseResult.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/Utils.h"
#include "donner/base/Vector2.h"
#include "donner/editor/ViewportSvgExport.h"
#include "donner/editor/ViewportState.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::editor {
namespace {

using svg::SVGDocument;
using svg::parser::SVGParser;

/// Upper bound on the fuzzer input size, matching other editor fuzzers.
constexpr std::size_t kMaxInputSize = 65536;

/// Fixed, deterministic render pane rect (screen px). Geometry is not the
/// point of this fuzzer - the XML/escaping path is - so a single stable
/// rect keeps every run's viewBox math identical and lets the fuzzer spend
/// its entropy on the SVG source instead. `Box2<int>` has no constexpr
/// constructor, so this is a plain function rather than a constant.
Recti RenderPaneRect() { return Recti(Vector2i(0, 0), Vector2i(400, 300)); }

/// A fixed identity-ish viewport: `screenToDocument` maps the render pane
/// rect above onto document-space `[0, 400] x [0, 300]`.
ViewportState FixedViewport() {
  ViewportState viewport;
  viewport.paneOrigin = Vector2d::Zero();
  viewport.paneSize = Vector2d(400.0, 300.0);
  return viewport;
}

}  // namespace

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > kMaxInputSize) {
    return 0;
  }

  FuzzedDataProvider provider(data, size);

  // Consume a couple of option bits up front so the export-option code paths
  // (background rect, empty overlay group) are exercised without eating into
  // the SVG source bytes' shape too much.
  ViewportExportOptions options;
  options.transparentBackground = provider.ConsumeBool();
  options.includeSelectionOverlay = provider.ConsumeBool();

  const std::vector<uint8_t> remaining = provider.ConsumeRemainingBytes<uint8_t>();
  // NOLINTNEXTLINE: Allow reinterpret_cast, matches SVGParser_fuzzer.cc.
  const std::string_view source(reinterpret_cast<const char*>(remaining.data()), remaining.size());

  ParseWarningSink warningSink = ParseWarningSink::Disabled();
  ParseResult<SVGDocument> parseResult = SVGParser::ParseSVG(source, warningSink);
  if (parseResult.hasError()) {
    // Malformed input is expected; never assert on the first parse failing.
    return 0;
  }

  const SVGDocument doc = std::move(parseResult).result();
  if (!doc.hasSourceStore()) {
    return 0;
  }

  const ViewportState viewport = FixedViewport();
  const Result<std::string, std::string> exportResult =
      ExportViewportAsSvg(doc, viewport, RenderPaneRect(), options);
  if (!exportResult.ok()) {
    // Expected refusal (e.g. external href/xlink:href reference) - not a bug.
    return 0;
  }

  // Stronger oracle, guarded behind a successful first parse and a
  // successful export: the exporter must never emit XML that the engine's
  // own untrusted-input parser rejects. A failure here is a serializer
  // escaping/correctness bug.
  ParseWarningSink reparseWarningSink = ParseWarningSink::Disabled();
  ParseResult<SVGDocument> reparseResult =
      SVGParser::ParseSVG(exportResult.value, reparseWarningSink);
  UTILS_RELEASE_ASSERT_MSG(!reparseResult.hasError(),
                           "ExportViewportAsSvg produced XML that failed to re-parse");

  return 0;
}

}  // namespace donner::editor
