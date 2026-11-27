#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "donner/base/xml/XMLTokenizer.h"
#include "donner/editor/CssSourceSyntax.h"
#include "donner/editor/TextEditorCore.h"
#include "donner/svg/renderer/tests/SvgCorpus.h"

namespace donner::editor {
namespace {

class CssSourceCorpus : public testing::TestWithParam<svg::tests::CorpusDocument> {};

// XML delimiters and comments are markup. Entity spellings and CDATA contents contribute CSS
// bytes, even when the CSS tokenizer sees a different string from the one in the source pane.
std::vector<bool> CssBytes(std::string_view raw) {
  std::vector<bool> bytes(raw.size());
  xml::Tokenize(raw, [&](xml::XMLToken token) {
    auto start = token.range.start.offset.value();
    auto end = token.range.end.offset.value();
    switch (token.type) {
      case xml::XMLTokenType::CData:
        start += 9;
        end -= 3;
        break;
      case xml::XMLTokenType::TextContent:
      case xml::XMLTokenType::EntityRef:
      case xml::XMLTokenType::ErrorRecovery: break;
      default: return;
    }
    std::fill(bytes.begin() + start, bytes.begin() + end, true);
  });
  return bytes;
}

TEST_P(CssSourceCorpus, PaintsEveryCssSourceByte) {
  std::ifstream input(GetParam().path, std::ios::binary);
  ASSERT_TRUE(input);
  const std::string file{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  TextEditorCore editor;
  editor.setSvgLanguageDefinition();
  editor.setText(file);
  editor.colorizeRange(0, editor.buffer().getTotalLines());
  const std::string source = editor.getText();

  // Compare visible glyphs by byte offset. The buffer represents a line break between glyph lines.
  const auto regions = FindSvgCssSourceRegions(source);
  if (regions.empty()) {
    return;
  }
  std::vector<ColorIndex> painted;
  painted.reserve(source.size() + 1);
  for (int i = 0; i < editor.buffer().getTotalLines(); ++i) {
    for (const auto& glyph : editor.buffer().getLineGlyphs(i)) {
      painted.push_back(glyph.colorIndex);
    }
    painted.push_back(ColorIndex::Default);
  }
  std::size_t previousEnd = 0;
  for (const auto& region : regions) {
    ASSERT_LE(previousEnd, region.startOffset);
    ASSERT_LE(region.endOffset, source.size());
    const std::string_view raw(source.data() + region.startOffset,
                               region.endOffset - region.startOffset);
    auto unpainted = CssBytes(raw);
    ClassifySvgCssRegion(source, region, [&](CssColorSpan span) {
      ASSERT_GE(span.startOffset, region.startOffset);
      ASSERT_LT(span.startOffset, span.endOffset);
      ASSERT_LE(span.endOffset, region.endOffset);
      for (auto pos = span.startOffset; pos < span.endOffset; ++pos) {
        ASSERT_TRUE(unpainted[pos - region.startOffset]) << pos;
        unpainted[pos - region.startOffset] = false;
        if (source[pos] != '\n') {
          ASSERT_EQ(painted[pos], span.color) << pos;
        }
      }
    });
    ASSERT_EQ(std::find(unpainted.begin(), unpainted.end(), true), unpainted.end());
    previousEnd = region.endOffset;
  }
}

INSTANTIATE_TEST_SUITE_P(Artwork, CssSourceCorpus, testing::ValuesIn(svg::tests::CorpusDocuments()),
                         [](const auto& info) { return info.param.name; });

}  // namespace
}  // namespace donner::editor
