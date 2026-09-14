/// @file
/// Temporary preview outcomes distinguish consumer resource limits from shared decode waits.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/editor/AsyncRenderer.h"
#include "donner/svg/resources/FontCatalog.h"
#include "donner/svg/resources/FontManager.h"

namespace donner::editor {
namespace {

TEST(TemporaryFontResourcesTest, RetainedBudgetWaitIsTerminalInsteadOfRequeued) {
  svg::FontCatalog catalog;
  Registry registry;
  svg::FontManager manager(registry, svg::FontManager::kDefaultMaximumLoadedFontBytes, 1);
  manager.setFontProvider(&catalog);
  const auto fallback = manager.fallbackFont();
  EXPECT_EQ(manager.findFont("Inter"), fallback);
  const auto dependencies = manager.faceDependencies();
  ASSERT_THAT(dependencies, testing::SizeIs(1));
  EXPECT_EQ(dependencies[0].waitReason, svg::FontFaceWaitReason::RetainedBudget);
  EXPECT_EQ(ClassifyTemporaryFontResources(dependencies,
                                           svg::FontResourcePreflight::Status::PendingFonts),
            SampleThumbnailRenderOutcome::ResourceLimit);
  EXPECT_EQ(manager.needsResourceRefresh(), false);
  EXPECT_EQ(manager.refreshPendingFonts(), false);
  EXPECT_EQ(manager.compressedFontDecompressionAttempts(), 0u);
  EXPECT_EQ(ClassifyTemporaryFontResources(manager.faceDependencies(),
                                           svg::FontResourcePreflight::Status::Ready),
            SampleThumbnailRenderOutcome::ResourceLimit);
}

TEST(TemporaryFontResourcesTest, SharedDecodeSlotWaitRemainsPending) {
  svg::FontCatalog catalog;
  auto occupied = catalog.tryAcquireFace("Inter", {});
  Registry registry;
  svg::FontManager manager(registry);
  manager.setFontProvider(&catalog);
  EXPECT_EQ(manager.findFont("Inter"), manager.fallbackFont());
  ASSERT_THAT(manager.faceDependencies(), testing::SizeIs(1));
  EXPECT_EQ(manager.faceDependencies()[0].waitReason, svg::FontFaceWaitReason::SharedDecodeSlot);
  EXPECT_EQ(ClassifyTemporaryFontResources(manager.faceDependencies(),
                                           svg::FontResourcePreflight::Status::Ready),
            SampleThumbnailRenderOutcome::FontsPending);
  occupied.reservation.reset();
  EXPECT_EQ(manager.refreshPendingFonts(), true);
  EXPECT_EQ(ClassifyTemporaryFontResources(manager.faceDependencies(),
                                           svg::FontResourcePreflight::Status::Ready),
            SampleThumbnailRenderOutcome::Rendered);
}

TEST(TemporaryFontResourcesTest, IncompleteCollectionCannotPublishAnEmptyDependencyPreview) {
  for (const auto status : {svg::FontResourcePreflight::Status::NeedsRender,
                            svg::FontResourcePreflight::Status::InvalidTarget,
                            svg::FontResourcePreflight::Status::Unavailable}) {
    EXPECT_EQ(ClassifyTemporaryFontResources({}, status), SampleThumbnailRenderOutcome::RenderError)
        << status;
  }
  EXPECT_EQ(ClassifyTemporaryFontResources({}, svg::FontResourcePreflight::Status::ResourceLimit),
            SampleThumbnailRenderOutcome::ResourceLimit);
}

}  // namespace
}  // namespace donner::editor
