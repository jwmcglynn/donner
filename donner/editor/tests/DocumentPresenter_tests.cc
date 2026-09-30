#include "donner/editor/DocumentPresenter.h"

#include "donner/editor/EditorApp.h"
#include "donner/editor/tests/FramePresentationTestAccess.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

std::shared_ptr<const FramePresentation> Frame(std::uint64_t id, double zoom = 1.0) {
  EditorApp app;
  if (!app.loadFromString(
          R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"/>)")) {
    return nullptr;
  }
  const PresentationIdentity identity{.captureId = 1,
                                      .documentGeneration = app.document().documentGeneration(),
                                      .version = app.document().currentFrameVersion(),
                                      .geometryRevision = app.document().nonTransformRevision()};
  const auto capture = CapturedPresentation::Capture(app.document().document(), identity, {});
  FramePresentationInput input;
  input.frameId = id;
  input.documentIdentity = identity;
  input.viewport.documentViewBox = Box2d::FromXYWH(0.0, 0.0, 100.0, 100.0);
  input.viewport.paneSize = Vector2d(200.0, 100.0);
  input.viewport.zoom = zoom;
  input.paneClipRect = Box2d::FromXYWH(0.0, 0.0, 200.0, 100.0);
  return FramePresentation::Build(FramePresentationTestAccess::resources(capture, {}), input);
}

TEST(DocumentPresenterTest, InstallsOneOwnedPlanForEveryPass) {
  std::vector<std::shared_ptr<const FramePresentation>> installed;
  DocumentPresenter presenter([&](auto frame) { installed.push_back(std::move(frame)); });
  const auto frame = Frame(1, 2.5);
  ASSERT_THAT(frame, testing::Ne(nullptr));
  ASSERT_THAT(presenter.present(frame), testing::IsTrue());
  ASSERT_THAT(installed, testing::ElementsAre(frame));
  EXPECT_THAT(presenter.currentFrame(), testing::Eq(frame));
  EXPECT_THAT(installed.front()->viewport().zoom, testing::Eq(2.5));
  EXPECT_THAT(installed.front()->identity(), testing::Eq(frame->identity()));
}

TEST(DocumentPresenterTest, LateAndConflictingPlansCannotReplaceTheInstalledFrame) {
  std::vector<std::shared_ptr<const FramePresentation>> installed;
  DocumentPresenter presenter([&](auto frame) { installed.push_back(std::move(frame)); });
  const auto older = Frame(1);
  const auto current = Frame(2);
  ASSERT_THAT(presenter.present(current), testing::IsTrue());
  EXPECT_THAT(presenter.present(older), testing::IsFalse());
  EXPECT_THAT(presenter.present(Frame(2, 3.0)), testing::IsFalse());
  EXPECT_THAT(installed, testing::ElementsAre(current));
  EXPECT_THAT(presenter.currentFrame(), testing::Eq(current));
  EXPECT_THAT(presenter.present(current), testing::IsTrue());
  EXPECT_THAT(installed, testing::ElementsAre(current, current));
}

TEST(DocumentPresenterTest, ClearingRemovesTheCompletePresentation) {
  std::vector<std::shared_ptr<const FramePresentation>> installed;
  DocumentPresenter presenter([&](auto frame) { installed.push_back(std::move(frame)); });
  const auto frame = Frame(1);
  ASSERT_THAT(presenter.present(frame), testing::IsTrue());
  EXPECT_THAT(presenter.present(nullptr), testing::IsTrue());
  EXPECT_THAT(installed, testing::ElementsAre(frame, nullptr));
  EXPECT_THAT(presenter.currentFrame(), testing::Eq(nullptr));
}

TEST(DocumentPresenterTest, InvalidCameraCannotProduceADrawablePlan) {
  EXPECT_THAT(Frame(1, 0.0), testing::Eq(nullptr));
  EXPECT_THAT(Frame(1, std::numeric_limits<double>::infinity()), testing::Eq(nullptr));
  EXPECT_THAT(Frame(0), testing::Eq(nullptr));
}

}  // namespace
}  // namespace donner::editor
