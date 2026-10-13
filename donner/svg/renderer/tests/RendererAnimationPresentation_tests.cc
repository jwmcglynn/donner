#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/encoding/Base64.h"
#include "donner/svg/SVGUseElement.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"
#include "donner/svg/resources/ResourceLoaderInterface.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {
namespace {

SVGDocument ParsePresentation(std::string_view source) {
  return instantiateSubtree(source, parser::SVGParser::Options(), Vector2i(16, 16));
}

TEST(RendererAnimationPresentation, PatternTileWidthRendersAndRestores) {
  auto document = ParsePresentation(R"svg(
    <defs><pattern id="p" patternUnits="userSpaceOnUse" width="4" height="16">
      <set attributeName="width" to="8" dur="1s"/>
      <rect width="2" height="16"/>
    </pattern></defs><rect width="16" height="16" fill="url(#p)"/>)svg");
  document.setTime(0.5);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@......@@......"));
  document.setTime(2);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@..@@..@@..@@.."));
}

TEST(RendererAnimationPresentation, FilterRegionWidthRendersAndRestores) {
  auto document = ParsePresentation(R"svg(
    <defs><filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="4" height="16">
      <set attributeName="width" to="12" dur="1s"/><feOffset/>
    </filter></defs><rect width="16" height="16" filter="url(#f)"/>)svg");
  document.setTime(0.5);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@@@@@@@@@@@...."));
  document.setTime(2);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@@@............"));
}

TEST(RendererAnimationPresentation, FilterPrimitiveWidthRendersAndRestores) {
  auto document = ParsePresentation(R"svg(
    <defs><filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="16" height="16">
      <feFlood x="0" y="0" width="4" height="16" flood-color="black">
        <set attributeName="width" to="12" dur="1s"/>
      </feFlood>
    </filter></defs><rect width="16" height="16" filter="url(#f)"/>)svg");
  document.setTime(0.5);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@@@@@@@@@@@...."));
  document.setTime(2);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@@@............"));
}

TEST(RendererAnimationPresentation, TextPositionMatchesAnIndependentStaticSample) {
  auto document = ParsePresentation(R"svg(
    <text x="0" y="12" font-size="12">H<set attributeName="x" to="8" dur="1s"/></text>)svg");
  auto expected = ParsePresentation(R"(<text x="8" y="12" font-size="12">H</text>)");
  auto base = ParsePresentation(R"(<text x="0" y="12" font-size="12">H</text>)");
  document.setTime(0.5);
  const auto expectedAscii = RendererTestUtils::renderToAsciiImage(expected).generated;
  EXPECT_THAT(expectedAscii, testing::HasSubstr("@"));
  EXPECT_EQ(RendererTestUtils::renderToAsciiImage(document).generated, expectedAscii);
  document.setTime(2);
  EXPECT_EQ(RendererTestUtils::renderToAsciiImage(document).generated,
            RendererTestUtils::renderToAsciiImage(base).generated);
}

TEST(RendererAnimationPresentation, EditedPatternWidthRestoresAuthorCascadePriority) {
  auto document = ParsePresentation(R"svg(
    <style>pattern { width: 6px; }</style>
    <defs><pattern id="p" patternUnits="userSpaceOnUse" width="4" height="16">
      <set attributeName="width" to="8" dur="1s"/><rect width="2" height="16"/>
    </pattern></defs><rect width="16" height="16" fill="url(#p)"/>)svg");
  document.setTime(0.5);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@......@@......"));
  document.querySelector("#p")->setAttribute("width", "12");
  document.setTime(2);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@....@@....@@.."));
}

TEST(RendererAnimationPresentation, MotionUsesCssDefinedPathGeometry) {
  auto document = ParsePresentation(R"svg(
    <defs><path id="route" d="M0 0L8 0" style="d: 'M0 0L0 8'"/></defs>
    <rect width="4" height="4"><animateMotion dur="2s"><mpath href="#route"/></animateMotion></rect>)svg");
  auto expected = ParsePresentation(R"(<rect y="4" width="4" height="4"/>)");
  document.setTime(1);
  EXPECT_EQ(RendererTestUtils::renderToAsciiImage(document).generated,
            RendererTestUtils::renderToAsciiImage(expected).generated);
}

TEST(RendererAnimationPresentation, TextLengthAndRotationMatchStaticSamples) {
  struct Case {
    std::string_view attribute;
    std::string_view value;
  };
  for (const Case item : {Case{"textLength", "12"}, Case{"rotate", "15"}}) {
    SCOPED_TRACE(item.attribute);
    const std::string opening = "<text x='2' y='12' font-size='12' lengthAdjust='spacingAndGlyphs'";
    auto document =
        ParsePresentation(opening + ">H<set attributeName='" + std::string(item.attribute) +
                          "' to='" + std::string(item.value) + "' dur='1s'/></text>");
    auto expected = ParsePresentation(opening + " " + std::string(item.attribute) + "='" +
                                      std::string(item.value) + "'>H</text>");
    auto base = ParsePresentation(opening + ">H</text>");
    document.setTime(0.5);
    const auto expectedAscii = RendererTestUtils::renderToAsciiImage(expected).generated;
    EXPECT_THAT(expectedAscii, testing::ContainsRegex("[^.\\n]"));
    EXPECT_EQ(RendererTestUtils::renderToAsciiImage(document).generated, expectedAscii);
    document.setTime(2);
    EXPECT_EQ(RendererTestUtils::renderToAsciiImage(document).generated,
              RendererTestUtils::renderToAsciiImage(base).generated);
  }
}

TEST(RendererAnimationPresentation, UseHrefSwitchesAndRestoresWithoutChangingSource) {
  auto document = ParsePresentation(R"svg(
    <defs><rect id="a" width="4" height="16"/><rect id="b" x="8" width="4" height="16"/></defs>
    <use href="#a"><set attributeName="href" to="#b" begin="0.5s" dur="1s"/></use>)svg");
  const std::string source(document.source());
  for (int cycle = 0; cycle < 3; ++cycle) {
    document.setTime(1);
    EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
                testing::StartsWith("........@@@@...."));
    document.setTime(2);
    EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
                testing::StartsWith("@@@@............"));
  }
  EXPECT_EQ(document.source(), source);
}

TEST(RendererAnimationPresentation, GradientHrefSwitchesInheritedStops) {
  auto document = ParsePresentation(R"svg(
    <defs><linearGradient id="a"><stop stop-color="black"/></linearGradient>
      <linearGradient id="b"><stop stop-color="red"/></linearGradient>
      <linearGradient id="g" href="#a"/>
      <set href="#g" attributeName="href" to="#b" begin="0.5s" dur="1s"/>
    </defs><rect width="16" height="16" fill="url(#g)"/>)svg");
  document.setTime(1);
  EXPECT_THAT(test::PixelAt(RenderDocumentWithActiveBackend(document), 8, 8),
              test::Rgba(255, 0, 0, 255));
  document.setTime(2);
  EXPECT_THAT(test::PixelAt(RenderDocumentWithActiveBackend(document), 8, 8),
              test::Rgba(0, 0, 0, 255));
}

TEST(RendererAnimationPresentation, PatternHrefSwitchesInheritedContent) {
  auto document = ParsePresentation(R"svg(
    <defs><pattern id="a" patternUnits="userSpaceOnUse" width="16" height="16"><rect width="4" height="16"/></pattern>
      <pattern id="b" patternUnits="userSpaceOnUse" width="16" height="16"><rect x="8" width="4" height="16"/></pattern>
      <pattern id="p" href="#a"/>
      <set href="#p" attributeName="href" to="#b" begin="0.5s" dur="1s"/>
    </defs><rect width="16" height="16" fill="url(#p)"/>)svg");
  document.setTime(1);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("........@@@@...."));
  document.setTime(2);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@@@............"));
}

TEST(RendererAnimationPresentation, AnimatedMpathHrefChangesTheSameMotionSample) {
  auto document = ParsePresentation(R"svg(
    <defs><path id="a" d="M0 0L8 0"/><path id="b" d="M0 0L0 8"/></defs>
    <rect width="4" height="4"><animateMotion dur="2s"><mpath id="m" href="#a"/></animateMotion></rect>
    <set href="#m" attributeName="href" to="#b" begin="0.5s" dur="1s"/>)svg");
  auto expected = ParsePresentation(R"(<rect y="4" width="4" height="4"/>)");
  document.setTime(1);
  EXPECT_EQ(RendererTestUtils::renderToAsciiImage(document).generated,
            RendererTestUtils::renderToAsciiImage(expected).generated);
}

TEST(RendererAnimationPresentation, TextPathHrefSwitchesLayout) {
  auto document = ParsePresentation(R"svg(
    <defs><path id="a" d="M0 12H16"/><path id="b" d="M0 16H16"/></defs>
    <text font-size="12"><textPath id="t" href="#a">H</textPath></text>
    <set href="#t" attributeName="href" to="#b" begin="0.5s" dur="1s"/>)svg");
  auto expected = ParsePresentation(R"svg(
    <defs><path id="b" d="M0 16H16"/></defs>
    <text font-size="12"><textPath href="#b">H</textPath></text>)svg");
  document.setTime(1);
  EXPECT_EQ(RendererTestUtils::renderToAsciiImage(document).generated,
            RendererTestUtils::renderToAsciiImage(expected).generated);
}

TEST(RendererAnimationPresentation, ConstantImageReferenceKeepsItsDecodedResource) {
  auto document = ParsePresentation(R"svg(
    <image id="i" width="16" height="16"><set attributeName="href" dur="10s"
      to="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAEElEQVR4nGP4z8AARAwQCgAf7gP9i18U1AAAAABJRU5ErkJggg=="/>
    </image>)svg");
  auto& registry = document.registry();
  registry.ctx().erase<components::ResourceManagerContext>();
  registry.ctx()
      .emplace<components::ResourceManagerContext>(registry, 256)
      .setProcessingMode(ProcessingMode::DynamicInteractive);
  const Entity target = document.querySelector("#i")->entityHandle().entity();
  uint64_t firstRevision = 0;
  for (int frame = 0; frame < 12; ++frame) {
    SCOPED_TRACE(frame);
    document.setTime(1 + frame / 60.0);
    EXPECT_THAT(test::PixelAt(RenderDocumentWithActiveBackend(document), 8, 8),
                test::Rgba(255, 0, 0, 255));
    const auto* loaded = registry.try_get<components::LoadedImageComponent>(target);
    ASSERT_THAT(loaded, testing::Ne(nullptr));
    if (frame == 0) {
      firstRevision = loaded->revision();
    }
    EXPECT_EQ(loaded->revision(), firstRevision);
  }
}

TEST(RendererAnimationPresentation, NestedReferenceMutationDoesNotLeakShadowEntities) {
  auto document = ParsePresentation(R"svg(
    <defs><pattern id="p" patternUnits="userSpaceOnUse" width="4" height="4"><rect width="2" height="4"/></pattern>
      <rect id="shape" width="4" height="16" fill="url(#p)"/>
      <g id="a"><use href="#shape"/></g><g id="b"><use href="#shape" x="8"/></g>
    </defs><use id="u" href="#a"><set attributeName="href" to="#b" dur="10s"/></use>)svg");
  std::size_t firstCount = 0;
  for (int frame = 0; frame < 5; ++frame) {
    document.setTime(1 + frame / 60.0);
    (void)RendererTestUtils::renderToAsciiImage(document);
    const auto count = document.registry().view<components::ShadowEntityComponent>().size();
    ASSERT_GT(count, 0);
    if (frame == 0) {
      firstCount = count;
    }
    EXPECT_EQ(count, firstCount);
    document.querySelector("#u")->setId("u");
  }
}

class AnimationRecordingLoader final : public ResourceLoaderInterface {
public:
  std::vector<std::string> requests;
  std::variant<std::vector<uint8_t>, ResourceLoaderError> fetchExternalResource(
      std::string_view url) override {
    requests.emplace_back(url);
    return ResourceLoaderError::NotFound;
  }
};

TEST(RendererAnimationPresentation, AnimatedResourceReferencesRespectSecureLoading) {
  for (const auto mode : {ProcessingMode::DynamicInteractive, ProcessingMode::SecureAnimated}) {
    auto document = ParsePresentation(R"svg(
      <image id="i" width="16" height="16"/>
      <set href="#i" attributeName="href" to="https://example.invalid/image.png" dur="10s"/>)svg");
    auto loader = std::make_unique<AnimationRecordingLoader>();
    const auto& requests = loader->requests;
    auto& resources = document.registry().ctx().get<components::ResourceManagerContext>();
    resources.setResourceLoader(std::move(loader));
    resources.setProcessingMode(mode);
    document.setTime(1);
    (void)RenderDocumentWithActiveBackend(document);
    if (mode == ProcessingMode::DynamicInteractive) {
      EXPECT_THAT(requests, testing::ElementsAre("https://example.invalid/image.png"));
    } else {
      EXPECT_THAT(requests, testing::IsEmpty());
    }
  }
}

TEST(RendererAnimationPresentation, ForeignResourceTargetDoesNotLoadAnimatedHref) {
  auto document = ParsePresentation(R"svg(
    <image id="i" width="16" height="16"/>
    <set href="#i" attributeName="href" to="https://example.invalid/image.png" dur="10s"/>)svg");
  auto loader = std::make_unique<AnimationRecordingLoader>();
  const auto& requests = loader->requests;
  document.registry().ctx().get<components::ResourceManagerContext>().setResourceLoader(
      std::move(loader));
  document.querySelector("#i")->setAttribute("xmlns", "urn:foreign");
  document.setTime(1);
  (void)RenderDocumentWithActiveBackend(document);
  EXPECT_THAT(requests, testing::IsEmpty());
}

TEST(RendererAnimationPresentation, RepeatingImageReferencesReuseDecodedFrames) {
  auto document = ParsePresentation(R"svg(
    <image id="i" width="16" height="16">
      <set attributeName="href" begin="0;2;4;6;8;10;12;14" dur="1s"
        to="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAEElEQVR4nGP4z8AARAwQCgAf7gP9i18U1AAAAABJRU5ErkJggg=="/>
      <set attributeName="href" begin="1;3;5;7;9;11;13;15" dur="1s"
        to="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAD0lEQVR4nGNgYPgPRmAKABf2A/1+6zfzAAAAAElFTkSuQmCC"/>
    </image>)svg");
  auto& registry = document.registry();
  registry.ctx().erase<components::ResourceManagerContext>();
  registry.ctx()
      .emplace<components::ResourceManagerContext>(registry, 512)
      .setProcessingMode(ProcessingMode::DynamicInteractive);
  const Entity target = document.querySelector("#i")->entityHandle().entity();
  std::array<uint64_t, 2> revisions = {};
  for (int frame = 0; frame < 16; ++frame) {
    SCOPED_TRACE(frame);
    const int index = frame % 2;
    document.setTime(frame + 0.25);
    EXPECT_THAT(test::PixelAt(RenderDocumentWithActiveBackend(document), 8, 8),
                test::Rgba(index ? 0 : 255, 0, index ? 255 : 0, 255));
    const auto* loaded = registry.try_get<components::LoadedImageComponent>(target);
    ASSERT_THAT(loaded, testing::Ne(nullptr));
    if (frame < 2) {
      revisions[index] = loaded->revision();
    }
    EXPECT_EQ(loaded->revision(), revisions[index]);
  }
}

class AnimationImageLoader final : public ResourceLoaderInterface {
public:
  explicit AnimationImageLoader(bool blue) : blue_(blue) {}

  std::variant<std::vector<uint8_t>, ResourceLoaderError> fetchExternalResource(
      std::string_view) override {
    return DecodeBase64Data(blue_ ? "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAD0lEQVR4nGNgYP"
                                    "gPRmAKABf2A/1+6zfzAAAAAElFTkSuQmCC"
                                  : "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAEElEQVR4nGP4z8"
                                    "AARAwQCgAf7gP9i18U1AAAAABJRU5ErkJggg==")
        .result();
  }

private:
  bool blue_;
};

TEST(RendererAnimationPresentation, DecodedAnimationCacheRespectsSecureModeChanges) {
  auto document = ParsePresentation(R"svg(
    <image id="i" width="16" height="16"><animate attributeName="href"
      values="a.png;b.png" dur="2s" repeatCount="indefinite"/></image>)svg");
  auto& resources = document.registry().ctx().get<components::ResourceManagerContext>();
  resources.setResourceLoader(std::make_unique<AnimationImageLoader>(false));
  for (const double time : {0.25, 1.25}) {
    document.setTime(time);
    EXPECT_THAT(test::PixelAt(RenderDocumentWithActiveBackend(document), 8, 8),
                test::Rgba(255, 0, 0, 255));
  }
  resources.setProcessingMode(ProcessingMode::SecureAnimated);
  document.setTime(2.25);
  EXPECT_THAT(test::PixelAt(RenderDocumentWithActiveBackend(document), 8, 8),
              test::Rgba(0, 0, 0, 0));
}

TEST(RendererAnimationPresentation, DecodedAnimationCacheRespectsLoaderReplacement) {
  auto document = ParsePresentation(R"svg(
    <image width="16" height="16"><animate attributeName="href"
      values="a.png;b.png" dur="2s" repeatCount="indefinite"/></image>)svg");
  auto& resources = document.registry().ctx().get<components::ResourceManagerContext>();
  resources.setResourceLoader(std::make_unique<AnimationImageLoader>(false));
  for (const double time : {0.25, 1.25}) {
    document.setTime(time);
    EXPECT_THAT(test::PixelAt(RenderDocumentWithActiveBackend(document), 8, 8),
                test::Rgba(255, 0, 0, 255));
  }
  resources.setResourceLoader(std::make_unique<AnimationImageLoader>(true));
  for (const double time : {2.25, 3.25, 4.25}) {
    document.setTime(time);
    EXPECT_THAT(test::PixelAt(RenderDocumentWithActiveBackend(document), 8, 8),
                test::Rgba(0, 0, 255, 255));
  }
}

TEST(RendererAnimationPresentation, TypedReferenceEditEqualToPresentationBecomesBase) {
  auto document = ParsePresentation(R"svg(
    <defs><rect id="a" width="4" height="16"/><rect id="b" x="8" width="4" height="16"/></defs>
    <use id="u" href="#a"><set attributeName="href" to="#b" dur="1s"/></use>)svg");
  document.setTime(0.5);
  (void)RendererTestUtils::renderToAsciiImage(document);
  document.querySelector("#u")->cast<SVGUseElement>().setHref("#b");
  document.setTime(2);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("........@@@@...."));
}

TEST(RendererAnimationPresentation, MissingAndCyclicReferencesDisappearThenRestore) {
  for (const auto* target : {"#missing", "#u"}) {
    auto document = ParsePresentation(
        std::string("<defs><rect id='a' width='4' height='16'/></defs><use id='u' href='#a'>") +
        "<set attributeName='href' to='" + target + "' dur='1s'/></use>");
    document.setTime(0.5);
    EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
                testing::StartsWith("................"));
    document.setTime(2);
    EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
                testing::StartsWith("@@@@............"));
  }
}

TEST(RendererAnimationPresentation, XlinkReferenceAnimationRespectsHrefPrecedence) {
  for (bool preferred : {false, true}) {
    auto document =
        ParsePresentation(std::string("<defs><rect id='a' width='4' height='16'/><rect id='b' "
                                      "x='8' width='4' height='16'/></defs>") +
                          "<use xmlns:xlink='http://www.w3.org/1999/xlink' xlink:href='#a' " +
                          (preferred ? "href='#a'" : "") +
                          "><set attributeName='xlink:href' to='#b' dur='1s'/></use>");
    document.setTime(0.5);
    EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
                testing::StartsWith(preferred ? "@@@@............" : "........@@@@...."));
  }
}

}  // namespace
}  // namespace donner::svg
