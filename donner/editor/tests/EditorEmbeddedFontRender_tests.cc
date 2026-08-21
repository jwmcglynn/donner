/// @file
/// Product-contract coverage for fonts embedded in SVG documents opened by the editor.

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "donner/base/tests/Runfiles.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"

namespace donner::editor {
namespace {

std::string ReadRunfile(std::string_view path) {
  std::ifstream input(Runfiles::instance().Rlocation(std::string(path)));
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

TEST(EditorEmbeddedFontRenderTest, UntrustedDocumentFontMatchesTrustedReference) {
  const std::string source = ReadRunfile("donner/svg/renderer/testdata/z0rly_test6.svg");
  ASSERT_FALSE(source.empty());

  EditorApp referenceApp;
  ASSERT_TRUE(referenceApp.loadFromString(source));
  svg::TrustDocumentFontFacesForTesting(referenceApp.document().document());
  svg::Renderer referenceRenderer;
  referenceRenderer.draw(referenceApp.document().document());
  const svg::RendererBitmap reference = referenceRenderer.takeSnapshot();

  EditorApp documentApp;
  ASSERT_TRUE(documentApp.loadFromString(source));
  svg::Renderer documentRenderer;
  documentRenderer.draw(documentApp.document().document());
  const svg::RendererBitmap actual = documentRenderer.takeSnapshot();

  tests::CompareBitmapToBitmap(actual, reference, "z0rly_document_font",
                               tests::PixelmatchIdentityParams());
}

}  // namespace
}  // namespace donner::editor
