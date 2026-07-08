/// @file ShapeClipboardPaste_fuzzer.cc
///
/// Headless libFuzzer target for the editor's shape-clipboard PASTE path: the
/// deserialization of untrusted clipboard text and the string-spliced merge of
/// that text into a destination document.
///
/// Clipboard content is fully attacker-controlled (a paste can carry any bytes
/// from any source). The pipeline exercised here is:
///
///   1. \ref ShapeClipboardPayload::parse decodes the headered wire format:
///      a metadata block (bounds doubles, an escaped comma-separated id list, a
///      group-selection flag) followed by an SVG fragment. Header-less text is
///      accepted as a best-effort raw fragment.
///   2. \ref preparePaste validates the fragment, then hand-builds a merged
///      full-document SVG **as strings**: it wraps the untrusted fragment in a
///      `<g id="...">...</g>`, textually renames colliding ids, rewrites
///      `href` / `xlink:href` / `url(#id)` references, and splices the block
///      into the destination document source. This is the same
///      untrusted-content-into-hand-written-XML pattern as the exporter.
///
/// Oracles (both guarded behind successful upstream steps, never on failure):
///   - A payload that round-trips through \ref ShapeClipboardPayload::parse ->
///     toClipboardText -> parse must still yield a payload (the serializer must
///     not emit clipboard text its own parser rejects).
///   - When \ref preparePaste succeeds, its `mergedSource` must re-parse
///     cleanly with the engine parser. A crash or a re-parse failure means the
///     paste splice/id-rewrite produced invalid XML from untrusted content - a
///     real injection/correctness bug.
///
/// Parse and paste refusals (malformed clipboard, missing references, etc.) are
/// expected and never asserted on.

#include <fuzzer/FuzzedDataProvider.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "donner/base/ParseResult.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/Utils.h"
#include "donner/editor/EditorParseOptions.h"
#include "donner/editor/ShapeClipboardCommands.h"
#include "donner/editor/ShapeClipboardPayload.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::editor {
namespace {

using svg::SVGDocument;
using svg::parser::SVGParser;

/// Upper bound on the fuzzer input size.
constexpr std::size_t kMaxInputSize = 65536;

/// Fixed destination document (with an owned source store) that pastes merge
/// into. It declares a couple of ids so the paste path exercises collision
/// renaming and reference repair against a real destination.
constexpr std::string_view kDestinationSvg =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<svg xmlns=\"http://www.w3.org/2000/svg\" "
    "xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
    "width=\"200\" height=\"200\" viewBox=\"0 0 200 200\">\n"
    "  <defs><linearGradient id=\"grad\"><stop offset=\"0\" stop-color=\"red\"/>"
    "</linearGradient></defs>\n"
    "  <g id=\"layer\"><rect id=\"box\" x=\"10\" y=\"10\" width=\"40\" height=\"40\" "
    "fill=\"url(#grad)\"/></g>\n"
    "</svg>\n";

/// Parse the fixed destination document once per input. Returns std::nullopt if
/// it somehow fails to parse (it should not); the fuzzer then no-ops.
std::optional<SVGDocument> ParseDestination() {
  ParseWarningSink sink = ParseWarningSink::Disabled();
  ParseResult<SVGDocument> parsed = SVGParser::ParseSVG(kDestinationSvg, sink, EditorParseOptions());
  if (parsed.hasError()) {
    return std::nullopt;
  }
  return std::move(parsed).result();
}

/// Re-parse a merged paste source; assert it is valid XML the engine accepts.
void CheckMergedSourceReparses(std::string_view mergedSource) {
  ParseWarningSink sink = ParseWarningSink::Disabled();
  ParseResult<SVGDocument> reparsed = SVGParser::ParseSVG(mergedSource, sink, EditorParseOptions());
  UTILS_RELEASE_ASSERT_MSG(!reparsed.hasError(),
                           "preparePaste produced merged source that failed to re-parse");
}

}  // namespace

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > kMaxInputSize) {
    return 0;
  }

  // NOLINTNEXTLINE: Allow reinterpret_cast, matches SVGParser_fuzzer.cc.
  const std::string_view clipboardText(reinterpret_cast<const char*>(data), size);

  const std::optional<ShapeClipboardPayload> payload =
      ShapeClipboardPayload::parse(clipboardText);
  if (!payload.has_value()) {
    // Empty/whitespace clipboard, or header-only text: nothing to paste.
    return 0;
  }

  // Idempotence oracle: the serializer must not emit clipboard text its own
  // parser then rejects.
  const std::string reserialized = payload->toClipboardText();
  const std::optional<ShapeClipboardPayload> reparsedPayload =
      ShapeClipboardPayload::parse(reserialized);
  UTILS_RELEASE_ASSERT_MSG(reparsedPayload.has_value(),
                           "toClipboardText produced text that ShapeClipboardPayload::parse "
                           "rejected");

  std::optional<SVGDocument> destination = ParseDestination();
  if (!destination.has_value()) {
    return 0;
  }

  // Exercise both placement modes; each hand-builds a merged source string.
  for (const PastePlacement placement :
       {PastePlacement::EndOfRootOffset, PastePlacement::InFrontNoOffset}) {
    const PreparePasteResult result = preparePaste(*destination, *payload, placement);
    if (result.ok) {
      CheckMergedSourceReparses(result.mergedSource);
    }
  }

  return 0;
}

}  // namespace donner::editor
