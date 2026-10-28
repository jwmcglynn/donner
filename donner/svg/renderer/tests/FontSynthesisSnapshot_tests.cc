#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "donner/base/tests/Runfiles.h"
#include "donner/svg/renderer/PlacedTextGeometry.h"
#include "donner/svg/renderer/RenderSnapshot.h"
#include "donner/svg/renderer/tests/MockRendererInterface.h"
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/text/TextEngine.h"
#ifdef DONNER_TEXT_FULL
#include "donner/svg/resources/FontCatalog.h"
#endif

namespace donner::svg {
namespace {

using ::testing::_;
using ::testing::ElementsAreArray;
using ::testing::Eq;
using ::testing::IsEmpty;
using ::testing::NiceMock;
using ::testing::Not;
using ::testing::NotNull;
using ::testing::SizeIs;

std::vector<uint8_t> SnapshotSynthesisFont() {
  const std::string path =
      Runfiles::instance().Rlocation("third_party/resvg-test-suite/fonts/NotoSans-Regular.ttf");
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class SnapshotStyleProvider final : public FontFamilyProvider {
public:
  explicit SnapshotStyleProvider(std::shared_ptr<size_t> calls)
      : calls_(std::move(calls)), bytes_(SnapshotSynthesisFont()) {}

  std::vector<FontFamilyInfo> families() const override { return {{"sans-serif"}}; }
  bool hasFamily(std::string_view family) const override { return family == "sans-serif"; }
  std::vector<uint8_t> loadFamilyData(std::string_view family,
                                      const FontFaceRequest&) const override {
    ++*calls_;
    return hasFamily(family) ? bytes_ : std::vector<uint8_t>();
  }

private:
  std::shared_ptr<size_t> calls_;
  std::vector<uint8_t> bytes_;
};

struct CapturedStyleGlyph {
  RenderSnapshot snapshot;
  Path directOutline;
  double directAdvance = 0;
  bool allowSyntheticOblique = true;
  std::optional<FontFaceRequest> directRequest;
  bool directGenericSans = false;

  CapturedStyleGlyph() = default;
  CapturedStyleGlyph(const CapturedStyleGlyph&) = delete;
  CapturedStyleGlyph& operator=(const CapturedStyleGlyph&) = delete;
  CapturedStyleGlyph(CapturedStyleGlyph&&) noexcept = default;
  CapturedStyleGlyph& operator=(CapturedStyleGlyph&&) noexcept = default;
};

CapturedStyleGlyph CaptureStyleGlyph(const FontFamilyProvider* provider, std::string_view family,
                                     FontFaceRequest request, FontSynthesisStyle policy,
                                     bool rawFace = false, bool requireVariation = false) {
  CapturedStyleGlyph result;
  Registry registry;
  auto& fonts = registry.ctx().emplace<FontManager>(registry);
  fonts.setFontProvider(provider);
  auto& engine = registry.ctx().emplace<TextEngine>(fonts, registry);
  components::ComputedTextComponent text;
  components::ComputedTextComponent::TextSpan span;
  span.text = "A";
  span.end = 1;
  span.startsNewChunk = true;
  span.fontFamilies.emplace_back(family);
  span.fontStyle = request.style;
  span.fontWeight = request.weight;
  span.fontSynthesisStyle = policy;
  text.spans.push_back(span);
  TextLayoutParams layout;
  layout.fontSize = Lengthd(64, Lengthd::Unit::Px);
  layout.fontFamilies.emplace_back(family);
  layout.viewBox = Box2d(Vector2d::Zero(), Vector2d(200, 200));
  auto runs = engine.layout(text, layout);
  EXPECT_THAT(runs, SizeIs(1));
  if (runs.empty()) {
    return result;
  }
  EXPECT_THAT(runs.front().glyphs, SizeIs(1));
  if (runs.front().glyphs.empty()) {
    return result;
  }
  if (rawFace) {
    const auto data = fonts.fontData(runs.front().font);
    runs.front().font = fonts.loadFontData(data, FontDataTrust::Trusted);
  }
  const auto& run = runs.front();
  const auto& glyph = run.glyphs.front();
  const float scale = engine.scaleForEmToPixels(run.font, run.usedFontSizePx);
  result.directOutline = PlacedGlyphOutline(engine, run.font, run, glyph, scale, 0.1);
  result.directRequest = fonts.providerFaceRequest(run.font);
  result.directGenericSans = fonts.isGenericSansProviderFont(run.font);
  if (requireVariation) {
    EXPECT_THAT(fonts.sfntTable(run.font, "fvar"), testing::Optional(_));
  }
  result.directAdvance = glyph.xAdvance;
  result.allowSyntheticOblique = glyph.allowSyntheticOblique;
  auto prepared = std::make_shared<PreparedTextDraw>();
  prepared->runs = std::move(runs);
  TextParams params;
  params.preparedTextDraw = std::move(prepared);
  NiceMock<tests::MockRendererInterface> factory;
  RenderSnapshotRecorder recorder(result.snapshot, factory);
  recorder.drawText(registry, text, params);
  EXPECT_THAT(result.snapshot.liveRegistryReferenceCountForTesting(registry), Eq(0u));
  EXPECT_THAT(result.directOutline.points(), Not(IsEmpty()));
  return result;
}

void ExpectReplayMatchesDirect(const CapturedStyleGlyph& captured) {
  NiceMock<tests::MockRendererInterface> inspection;
  EXPECT_CALL(inspection, drawText(_, _, _))
      .WillOnce([&](Registry& registry, const components::ComputedTextComponent&,
                    const TextParams& params) {
        ASSERT_THAT(params.preparedTextDraw, NotNull());
        ASSERT_THAT(params.preparedTextDraw->runs, SizeIs(1));
        const auto& run = params.preparedTextDraw->runs.front();
        ASSERT_THAT(run.glyphs, SizeIs(1));
        const auto& glyph = run.glyphs.front();
        EXPECT_THAT(glyph.xAdvance, Eq(captured.directAdvance));
        EXPECT_THAT(glyph.allowSyntheticOblique, Eq(captured.allowSyntheticOblique));
        const auto& engine = registry.ctx().get<TextEngine>();
        const float scale = engine.scaleForEmToPixels(run.font, run.usedFontSizePx);
        const Path replayed = PlacedGlyphOutline(engine, run.font, run, glyph, scale, 0.1);
        EXPECT_THAT(replayed.points(), ElementsAreArray(captured.directOutline.points()));
      });
  captured.snapshot.replay(inspection);
}

TEST(FontSynthesisStyleSnapshotTest,
     PreparedAutoProviderStyleSurvivesSourceAndProviderDestruction) {
  auto calls = std::make_shared<size_t>(0);
  CapturedStyleGlyph captured;
  {
    SnapshotStyleProvider provider(calls);
    captured = CaptureStyleGlyph(&provider, "sans-serif", {400, FontStyle::Italic},
                                 FontSynthesisStyle::Auto);
  }
  ASSERT_THAT(captured.directRequest, testing::Optional(testing::Field(
                                          "style", &FontFaceRequest::style, FontStyle::Italic)));
  EXPECT_THAT(captured.directGenericSans, Eq(true));
  const size_t before = *calls;
  EXPECT_THAT(before, testing::Gt(0u));
  ExpectReplayMatchesDirect(captured);
  EXPECT_THAT(*calls, Eq(before));
}

TEST(FontSynthesisStyleSnapshotTest, PreparedNoneProviderStyleRetainsUprightGeometry) {
  auto calls = std::make_shared<size_t>(0);
  CapturedStyleGlyph captured;
  {
    SnapshotStyleProvider provider(calls);
    captured = CaptureStyleGlyph(&provider, "sans-serif", {400, FontStyle::Italic},
                                 FontSynthesisStyle::None);
  }
  const size_t before = *calls;
  ExpectReplayMatchesDirect(captured);
  EXPECT_THAT(*calls, Eq(before));
}

TEST(FontSynthesisStyleSnapshotTest, RawLoadedFaceWithoutProviderMetadataKeepsItsGeometry) {
  auto calls = std::make_shared<size_t>(0);
  CapturedStyleGlyph captured;
  {
    SnapshotStyleProvider provider(calls);
    captured = CaptureStyleGlyph(&provider, "sans-serif", {400, FontStyle::Italic},
                                 FontSynthesisStyle::Auto, true);
  }
  ExpectReplayMatchesDirect(captured);
}

#ifdef DONNER_TEXT_FULL
TEST(FontSynthesisStyleSnapshotTest, NativeWeightAxisSurvivesRawByteRemap) {
  CapturedStyleGlyph captured;
  {
    FontCatalog provider;
    captured = CaptureStyleGlyph(&provider, "Inter", {700, FontStyle::Normal},
                                 FontSynthesisStyle::None, false, true);
  }
  ASSERT_THAT(captured.directRequest,
              testing::Optional(testing::Field("weight", &FontFaceRequest::weight, 700)));
  ExpectReplayMatchesDirect(captured);
}
#endif

}  // namespace
}  // namespace donner::svg
