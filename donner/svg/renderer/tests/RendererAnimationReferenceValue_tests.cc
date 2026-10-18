#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {
namespace {

TEST(RendererAnimationReferenceValue, EmptyReferenceClearsAndRestoresUse) {
  const std::string content =
      "<defs><rect id='r' width='16' height='16'/></defs><use id='u' href='#r'/>";
  for (const std::string_view animation : {
           "<set href='#u' attributeName='href' to='' dur='2s'/>",
           "<animate href='#u' attributeName='href' to='' dur='2s'/>",
       }) {
    SCOPED_TRACE(animation);
    auto document = instantiateSubtree(content + std::string(animation),
                                       parser::SVGParser::Options(), Vector2i(16, 16));
    auto empty = instantiateSubtree("<rect width='0' height='0'/>", parser::SVGParser::Options(),
                                    Vector2i(16, 16));
    auto base = instantiateSubtree(content, parser::SVGParser::Options(), Vector2i(16, 16));
    document.setTime(1.5);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(empty),
        "empty-use-reference", editor::tests::PixelmatchIdentityParams());
    document.setTime(3);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(base),
        "restored-use-reference", editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationReferenceValue, MissingOrInvalidReferenceAnimationKeepsBase) {
  const std::string content =
      "<defs><rect id='r' width='16' height='16'/></defs><use id='u' href='#r'/>";
  for (const std::string_view animation : {
           "<set href='#u' attributeName='href' dur='2s'/>",
           "<animate href='#u' attributeName='href' by='#other' dur='2s'/>",
           "<animate href='#u' attributeName='href' to='' keyTimes='0;2' dur='2s'/>",
       }) {
    SCOPED_TRACE(animation);
    auto document = instantiateSubtree(content + std::string(animation),
                                       parser::SVGParser::Options(), Vector2i(16, 16));
    auto base = instantiateSubtree(content, parser::SVGParser::Options(), Vector2i(16, 16));
    document.setTime(1.5);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(base),
        "invalid-reference-preserves-base", editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationReferenceValue, TargetSkipsRetainedDetachedDuplicateId) {
  auto document = instantiateSubtree(
      "<rect id='target' width='4' height='16'/><rect id='target' x='8' width='4' height='16'/>"
      "<set href='#target' attributeName='width' to='8' dur='2s'/>",
      parser::SVGParser::Options(), Vector2i(16, 16));
  auto detached = document.querySelector("#target");
  ASSERT_THAT(detached, testing::Ne(std::nullopt));
  ASSERT_EQ(document.removeElement(*detached).applied, true);
  auto expected = instantiateSubtree("<rect x='8' width='8' height='16'/>",
                                     parser::SVGParser::Options(), Vector2i(16, 16));
  for (const bool release : {false, true}) {
    SCOPED_TRACE(release);
    if (release) {
      detached.reset();
    }
    document.setTime(1);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        "attached-animation-target", editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationReferenceValue, MotionSkipsRetainedDetachedDuplicateId) {
  auto document = instantiateSubtree(
      "<defs><path id='route' d='M0 0H0'/><path id='route' d='M0 0H8'/></defs>"
      "<rect width='4' height='16'><animateMotion dur='2s'><mpath href='#route'/>"
      "</animateMotion></rect>",
      parser::SVGParser::Options(), Vector2i(16, 16));
  auto detached = document.querySelector("#route");
  ASSERT_THAT(detached, testing::Ne(std::nullopt));
  ASSERT_EQ(document.removeElement(*detached).applied, true);
  document.setTime(1);
  auto expected = instantiateSubtree("<rect x='4' width='4' height='16'/>",
                                     parser::SVGParser::Options(), Vector2i(16, 16));
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "attached-motion-reference", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationReferenceValue, SyncbaseSkipsRetainedDetachedDuplicateId) {
  auto document = instantiateSubtree(
      "<rect width='4' height='16'><set id='clock' attributeName='opacity' to='1' dur='10s'/>"
      "<set id='clock' attributeName='opacity' to='1' dur='1s'/>"
      "<set attributeName='width' to='12' begin='clock.end' dur='2s'/></rect>",
      parser::SVGParser::Options(), Vector2i(16, 16));
  auto detached = document.querySelector("#clock");
  ASSERT_THAT(detached, testing::Ne(std::nullopt));
  ASSERT_EQ(document.removeElement(*detached).applied, true);
  document.setTime(1.5);
  auto expected = instantiateSubtree("<rect width='12' height='16'/>", parser::SVGParser::Options(),
                                     Vector2i(16, 16));
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "attached-syncbase-reference", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationReferenceValue, EventSkipsRetainedDetachedDuplicateId) {
  auto document = instantiateSubtree(
      "<rect id='button' width='4' height='16'/><rect id='button' width='4' height='16'>"
      "<set attributeName='width' to='12' begin='button.click' dur='2s'/></rect>",
      parser::SVGParser::Options(), Vector2i(16, 16));
  auto detached = document.querySelector("#button");
  ASSERT_THAT(detached, testing::Ne(std::nullopt));
  ASSERT_EQ(document.removeElement(*detached).applied, true);
  const auto attached = document.querySelector("#button");
  ASSERT_THAT(attached, testing::Ne(std::nullopt));
  document.setTime(0);
  EXPECT_EQ(document.dispatchAnimationEvent("click", *attached), true);
  document.setTime(1);
  auto expected = instantiateSubtree("<rect width='12' height='16'/>", parser::SVGParser::Options(),
                                     Vector2i(16, 16));
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "attached-event-reference", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationReferenceValue, EmptyResultNameClearsAndRestoresBinding) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filters are disabled in this configuration";
  }
  const std::string content =
      "<defs><filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='16' height='16'>"
      "<feFlood id='p' flood-color='red' result='red'/><feFlood flood-color='blue'/>"
      "<feOffset in='red'/></filter></defs>"
      "<rect width='16' height='16' filter='url(#f)'/>";
  auto document =
      instantiateSubtree(content + "<set href='#p' attributeName='result' to='' dur='2s'/>",
                         parser::SVGParser::Options(), Vector2i(16, 16));
  auto previous = instantiateSubtree("<rect width='16' height='16' fill='blue'/>",
                                     parser::SVGParser::Options(), Vector2i(16, 16));
  auto base = instantiateSubtree(content, parser::SVGParser::Options(), Vector2i(16, 16));
  document.setTime(1);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(previous),
      "empty-result-name", editor::tests::PixelmatchIdentityParams());
  document.setTime(3);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(base),
      "restored-result-name", editor::tests::PixelmatchIdentityParams());
}

}  // namespace
}  // namespace donner::svg
