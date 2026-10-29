#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <barrier>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <latch>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "donner/base/tests/Runfiles.h"
#include "donner/css/FontFace.h"
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

std::vector<uint8_t> SnapshotSynthesisFont(std::string_view name = "NotoSans-Regular.ttf") {
  const std::string path =
      Runfiles::instance().Rlocation("third_party/resvg-test-suite/fonts/" + std::string(name));
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
  span.start = 0;
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

TEST(FontSynthesisStyleSnapshotTest, ReconstructedSourceAddressCannotAliasAnotherFontLifetime) {
  RenderSnapshot snapshot;
  NiceMock<tests::MockRendererInterface> factory;
  RenderSnapshotRecorder recorder(snapshot, factory);
  std::optional<Registry> source;
  Registry* firstSource = nullptr;
  FontHandle firstFont;
  std::vector<Path> expected;
  std::weak_ptr<const void> previousOwner;
  std::vector<std::weak_ptr<const std::vector<uint8_t>>> sourceBytes;
  for (const char* filename : {"NotoSans-Regular.ttf", "NotoSans-Italic.ttf"}) {
    source.emplace();
    Registry& registry = *source;
    if (firstSource) {
      ASSERT_THAT(&registry, Eq(firstSource));
    } else {
      firstSource = &registry;
    }
    auto& fonts = registry.ctx().emplace<FontManager>(registry);
    auto& engine = registry.ctx().emplace<TextEngine>(fonts, registry);
    const auto bytes =
        std::make_shared<const std::vector<uint8_t>>(SnapshotSynthesisFont(filename));
    ASSERT_THAT(*bytes, Not(IsEmpty()));
    sourceBytes.push_back(bytes);
    css::FontFace face;
    face.familyName = "CapturedFace";
    face.sources.push_back({css::FontFaceSource::Kind::Data, bytes, {}, {}, true});
    fonts.addFontFace(face);
    components::ComputedTextComponent text;
    components::ComputedTextComponent::TextSpan span;
    span.text = "A";
    span.start = 0;
    span.end = 1;
    span.startsNewChunk = true;
    span.fontFamilies.emplace_back("CapturedFace");
    span.fontSynthesisStyle = FontSynthesisStyle::None;
    text.spans.push_back(span);
    TextLayoutParams layout;
    layout.fontFamilies.emplace_back("CapturedFace");
    layout.fontSize = Lengthd(64, Lengthd::Unit::Px);
    layout.viewBox = Box2d(Vector2d::Zero(), Vector2d(200, 200));
    auto runs = engine.layout(text, layout);
    ASSERT_THAT(runs, SizeIs(1));
    ASSERT_THAT(runs.front().glyphs, SizeIs(1));
    if (firstFont) {
      ASSERT_THAT(runs.front().font, Eq(firstFont));
    } else {
      firstFont = runs.front().font;
    }
    const auto& run = runs.front();
    expected.push_back(PlacedGlyphOutline(engine, run.font, run, run.glyphs.front(),
                                          engine.scaleForEmToPixels(run.font, run.usedFontSizePx),
                                          0.1));
    auto prepared = std::make_shared<PreparedTextDraw>();
    prepared->runs = std::move(runs);
    TextParams params;
    params.preparedTextDraw = std::move(prepared);
    recorder.drawText(registry, text, params);
    EXPECT_THAT(snapshot.liveRegistryReferenceCountForTesting(registry), Eq(0u));
    previousOwner = fonts.fontDomainIdentity().owner;
    source.reset();
    EXPECT_THAT(previousOwner.expired(), Eq(true));
    const std::weak_ptr<const void> empty;
    EXPECT_THAT(previousOwner.owner_before(empty) || empty.owner_before(previousOwner), Eq(true));
  }
  EXPECT_THAT(sourceBytes,
              testing::Each(testing::Truly([](const auto& bytes) { return bytes.expired(); })));
  ASSERT_THAT(expected, SizeIs(2));
  ASSERT_THAT(expected.front().points(), Not(IsEmpty()));
  ASSERT_THAT(expected.back().points(), Not(ElementsAreArray(expected.front().points())));
  NiceMock<tests::MockRendererInterface> inspection;
  Registry* firstReplay = nullptr;
  size_t index = 0;
  EXPECT_CALL(inspection, drawText(_, _, _))
      .Times(2)
      .WillRepeatedly([&](Registry& registry, const components::ComputedTextComponent&,
                          const TextParams& params) {
        ASSERT_THAT(index, testing::Lt(expected.size()));
        if (firstReplay) {
          EXPECT_THAT(&registry, testing::Ne(firstReplay));
        } else {
          firstReplay = &registry;
        }
        ASSERT_THAT(params.preparedTextDraw, NotNull());
        ASSERT_THAT(params.preparedTextDraw->runs, SizeIs(1));
        const auto& run = params.preparedTextDraw->runs.front();
        ASSERT_THAT(run.glyphs, SizeIs(1));
        const auto& engine = registry.ctx().get<TextEngine>();
        const Path outline =
            PlacedGlyphOutline(engine, run.font, run, run.glyphs.front(),
                               engine.scaleForEmToPixels(run.font, run.usedFontSizePx), 0.1);
        EXPECT_THAT(outline.points(), ElementsAreArray(expected[index].points()));
        ++index;
      });
  snapshot.replay(inspection);
  EXPECT_THAT(index, Eq(2u));
}

TEST(FontSynthesisStyleSnapshotTest, SameBytesKeepDistinctRequestsAcrossAllPreparedFontSlots) {
  RenderSnapshot snapshot;
  NiceMock<tests::MockRendererInterface> inspection;
  std::vector<Path> outlines;
  std::vector<FontFaceRequest> requests;
  std::vector<FontHandle> sourceFaces;
  {
    Registry source;
    auto calls = std::make_shared<size_t>(0);
    SnapshotStyleProvider provider(calls);
    auto& fonts = source.ctx().emplace<FontManager>(source);
    fonts.setFontProvider(&provider);
    auto& engine = source.ctx().emplace<TextEngine>(fonts, source);
    auto decorationFamily = std::make_shared<components::DocumentResourceFamilyBudget>();
    RenderSnapshotRecorder recorder(snapshot, inspection);
    for (const FontStyle style : {FontStyle::Normal, FontStyle::Italic, FontStyle::Oblique}) {
      for (const FontSynthesisStyle policy : {FontSynthesisStyle::Auto, FontSynthesisStyle::None}) {
        components::ComputedTextComponent text;
        components::ComputedTextComponent::TextSpan span;
        span.text = "A";
        span.start = 0;
        span.end = 1;
        span.startsNewChunk = true;
        span.fontFamilies.emplace_back("sans-serif");
        span.fontStyle = style;
        span.fontSynthesisStyle = policy;
        text.spans.push_back(span);
        TextLayoutParams layout;
        layout.fontFamilies.emplace_back("sans-serif");
        layout.fontSize = Lengthd(64, Lengthd::Unit::Px);
        layout.viewBox = Box2d(Vector2d::Zero(), Vector2d(200, 200));
        auto runs = engine.layout(text, layout);
        ASSERT_THAT(runs, SizeIs(1));
        ASSERT_THAT(runs.front().glyphs, SizeIs(1));
        auto& run = runs.front();
        const FontHandle face = run.font;
        const FontFaceRequest request{400, style};
        ASSERT_THAT(fonts.providerFaceRequest(face), testing::Optional(request));
        if (!sourceFaces.empty()) {
          EXPECT_THAT(fonts.fontData(face), ElementsAreArray(fonts.fontData(sourceFaces.front())));
        }
        sourceFaces.push_back(face);
        requests.push_back(request);
        outlines.push_back(PlacedGlyphOutline(engine, face, run, run.glyphs.front(),
                                              engine.scaleForEmToPixels(face, run.usedFontSizePx),
                                              0.1));
        // Exercise all three remap slots with the same provider face. The run deliberately has
        // an upright face while the glyph and decoration retain the requested face.
        run.glyphs.front().font = face;
        run.font = fonts.findFont("sans-serif", 400, 0, 5);
        std::array<components::TextDecorationOrigin, 1> origins{};
        origins[0].lines = TextDecoration::Underline;
        origins[0].font = ResolvedTextFont{face, run.usedFontSizePx};
        text.spans.front().textDecoration = TextDecoration::Underline;
        text.spans.front().decorations =
            components::TextDecorationData::Create(decorationFamily, origins);
        ASSERT_THAT(text.spans.front().decorations, NotNull());
        auto prepared = std::make_shared<PreparedTextDraw>();
        prepared->runs = std::move(runs);
        TextParams params;
        params.preparedTextDraw = prepared;
        recorder.drawText(source, text, params);
      }
    }
    EXPECT_THAT(sourceFaces[0], testing::Ne(sourceFaces[2]));
    EXPECT_THAT(sourceFaces[2], testing::Ne(sourceFaces[4]));
    EXPECT_THAT(sourceFaces[0], Eq(sourceFaces[1]));
    ASSERT_THAT(outlines[2].points(), Not(ElementsAreArray(outlines[3].points())));
  }
  std::vector<FontHandle> replayFaces;
  EXPECT_CALL(inspection, drawText(_, _, _))
      .Times(6)
      .WillRepeatedly([&](Registry& replay, const components::ComputedTextComponent& text,
                          const TextParams& params) {
        const size_t index = replayFaces.size();
        ASSERT_THAT(index, testing::Lt(outlines.size()));
        ASSERT_THAT(params.preparedTextDraw, NotNull());
        const auto& run = params.preparedTextDraw->runs.front();
        const FontHandle face = run.glyphs.front().font;
        replayFaces.push_back(face);
        const auto& fonts = replay.ctx().get<FontManager>();
        EXPECT_THAT(fonts.providerFaceRequest(face), testing::Optional(requests[index]));
        EXPECT_THAT(fonts.providerFaceRequest(run.font), testing::Optional(FontFaceRequest{}));
        ASSERT_THAT(text.spans.front().decorations, NotNull());
        ASSERT_THAT(text.spans.front().decorations->origins(), SizeIs(1));
        const auto& decorationFont = text.spans.front().decorations->origins().front().font;
        ASSERT_THAT(decorationFont, testing::Optional(_));
        EXPECT_THAT(decorationFont->font, Eq(face));
        const auto& engine = replay.ctx().get<TextEngine>();
        const Path outline =
            PlacedGlyphOutline(engine, face, run, run.glyphs.front(),
                               engine.scaleForEmToPixels(face, run.usedFontSizePx), 0.1);
        EXPECT_THAT(outline.points(), ElementsAreArray(outlines[index].points()));
      });
  snapshot.replay(inspection);
  ASSERT_THAT(replayFaces, SizeIs(6));
  EXPECT_THAT(replayFaces[0], Eq(replayFaces[1]));
  EXPECT_THAT(replayFaces[0], testing::Ne(replayFaces[2]));
  EXPECT_THAT(replayFaces[2], testing::Ne(replayFaces[4]));
}

TEST(FontSynthesisStyleSnapshotTest, RepeatedAndConcurrentReplayOwnTheirFontDomains) {
  auto calls = std::make_shared<size_t>(0);
  CapturedStyleGlyph captured;
  {
    SnapshotStyleProvider provider(calls);
    captured = CaptureStyleGlyph(&provider, "sans-serif", {400, FontStyle::Oblique},
                                 FontSynthesisStyle::Auto);
  }
  const size_t before = *calls;
  ExpectReplayMatchesDirect(captured);
  ExpectReplayMatchesDirect(captured);
  std::barrier bothDomainsLive(2);
  std::latch firstReplayFinished(1);
  std::array<const Registry*, 2> domains{};
  std::array<FontHandle, 2> faces{};
  std::array<const uint8_t*, 2> fontBytes{};
  const auto replay = [&](size_t index) {
    NiceMock<tests::MockRendererInterface> inspection;
    EXPECT_CALL(inspection, drawText(_, _, _))
        .WillOnce([&](Registry& registry, const components::ComputedTextComponent&,
                      const TextParams& params) {
          domains[index] = &registry;
          if (params.preparedTextDraw && !params.preparedTextDraw->runs.empty()) {
            faces[index] = params.preparedTextDraw->runs.front().font;
            fontBytes[index] = registry.ctx().get<FontManager>().fontData(faces[index]).data();
          }
          // All assertions that can return from this callback follow the rendezvous. The outer
          // replay function still signals retirement if a later ASSERT ends this callback early.
          bothDomainsLive.arrive_and_wait();
          EXPECT_THAT(domains[0], testing::Ne(domains[1]));
          EXPECT_THAT(std::pair(domains[0], faces[0]),
                      testing::Ne(std::pair(domains[1], faces[1])));
          EXPECT_THAT(fontBytes[0], NotNull());
          EXPECT_THAT(fontBytes[1], NotNull());
          EXPECT_THAT(fontBytes[0], testing::Ne(fontBytes[1]));
          // The domains and byte loaders overlap. The Full backend's shared FreeType library
          // requires serialized face creation/destruction, so its first outline extraction waits
          // for the other replay's face to retire. This tests namespace isolation, not that backend
          // library's concurrency contract.
          if (index != 0) {
            firstReplayFinished.wait();
          }
          ASSERT_THAT(params.preparedTextDraw, NotNull());
          const auto& run = params.preparedTextDraw->runs.front();
          const auto& engine = registry.ctx().get<TextEngine>();
          const Path outline =
              PlacedGlyphOutline(engine, run.font, run, run.glyphs.front(),
                                 engine.scaleForEmToPixels(run.font, run.usedFontSizePx), 0.1);
          EXPECT_THAT(outline.points(), ElementsAreArray(captured.directOutline.points()));
        });
    captured.snapshot.replay(inspection);
    if (index == 0) {
      firstReplayFinished.count_down();
    }
  };
  std::thread first(replay, 0);
  std::thread second(replay, 1);
  first.join();
  second.join();
  EXPECT_THAT(*calls, Eq(before));
}

TEST(FontSynthesisStyleSnapshotTest, RefusedDomainDoesNotBorrowPreparedFaceOrSource) {
  Registry registry;
  auto& fonts = registry.ctx().emplace<FontManager>(registry);
  const FontHandle font = fonts.loadFontData(SnapshotSynthesisFont(), FontDataTrust::Trusted);
  ASSERT_THAT(static_cast<bool>(font), Eq(true));
  auto prepared = std::make_shared<PreparedTextDraw>();
  TextRun run;
  run.font = font;
  run.glyphs.push_back(TextGlyph{.glyphIndex = 1, .font = font});
  prepared->runs.push_back(run);
  TextParams params;
  params.preparedTextDraw = prepared;
  RenderSnapshot snapshot;
  NiceMock<tests::MockRendererInterface> inspection;
  components::ComputedTextComponent text;
  text.spans.emplace_back();
  std::array<components::TextDecorationOrigin, 1> origins{};
  origins[0].lines = TextDecoration::Underline;
  origins[0].font = ResolvedTextFont{font, 16.0f};
  text.spans.back().textDecoration = TextDecoration::Underline;
  text.spans.back().decorations = components::TextDecorationData::Create(
      std::make_shared<components::DocumentResourceFamilyBudget>(), origins);
  ASSERT_THAT(text.spans.back().decorations, NotNull());
  RenderSnapshotRecorder recorder(snapshot, inspection, 0);
  recorder.drawText(registry, text, params);
  EXPECT_THAT(snapshot.fontPayloadLimitExceeded(), Eq(true));
  EXPECT_THAT(snapshot.liveRegistryReferenceCountForTesting(registry), Eq(0u));
  EXPECT_CALL(inspection, drawText(_, _, _))
      .WillOnce([&](Registry& replay, const components::ComputedTextComponent& replayText,
                    const TextParams& replayParams) {
        EXPECT_THAT(&replay, testing::Ne(&registry));
        ASSERT_THAT(replayText.spans, SizeIs(1));
        ASSERT_THAT(replayText.spans.front().decorations, NotNull());
        const auto& origin = replayText.spans.front().decorations->origins().front();
        ASSERT_THAT(origin.font, testing::Optional(_));
        EXPECT_THAT(static_cast<bool>(origin.font->font), Eq(false));
        ASSERT_THAT(replayParams.preparedTextDraw, NotNull());
        const auto& replayRun = replayParams.preparedTextDraw->runs.front();
        EXPECT_THAT(static_cast<bool>(replayRun.font), Eq(false));
        EXPECT_THAT(static_cast<bool>(replayRun.glyphs.front().font), Eq(false));
        EXPECT_THAT(replayRun.glyphs.front().glyphIndex, Eq(0));
      });
  snapshot.replay(inspection);
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
