#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <limits>
#include <optional>
#include <string>

#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/base/tests/RunfileGate.h"
#include "donner/editor/FramePresentation.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/editor/tests/FramePresentationTestAccess.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"

namespace donner::editor {
namespace {

using testing::AllOf;
using testing::ElementsAre;
using testing::Field;
using testing::IsEmpty;
using testing::IsFalse;
using testing::IsTrue;
using testing::Ne;
using testing::Optional;
using Event = svg::SVGAnimationEvent;

constexpr std::string_view kFixtureRoot = "donner/editor/tests/svg_suite_animation/";

class SvgSuiteAnimation : public testing::Test {
protected:
  void load(std::string_view fixture,
            svg::ProcessingMode mode = svg::ProcessingMode::DynamicInteractive) {
    const auto file = ::donner::tests::ReadRequiredRunfile(std::string(kFixtureRoot) +
                                                           std::string(fixture) + ".svg");
    DONNER_REQUIRE_RUNFILE(file);
    svg::parser::SVGParser::Options options;
    svg::SVGDocument::Settings settings;
    settings.processingMode = mode;
    ParseWarningSink warnings;
    auto parsed =
        svg::parser::SVGParser::ParseSVG(file.contents, warnings, options, std::move(settings));
    ASSERT_THAT(parsed, NoParseError());
    document_ = std::move(parsed.result());
    source_ = document_->source();
  }

  void compare(std::string_view reference) {
    // These independently derived oracles must never enter the helper's update mode.
    ASSERT_EQ(std::getenv("UPDATE_GOLDEN_IMAGES_DIR"), nullptr);
    const auto golden = ::donner::tests::ResolveRequiredRunfile(std::string(kFixtureRoot) +
                                                                std::string(reference) + ".png");
    DONNER_REQUIRE_RUNFILE(golden);
    svg::Renderer renderer;
    renderer.draw(*document_);
    const auto bitmap = renderer.takeSnapshot();
    tests::CompareBitmapToGolden(bitmap, golden.path, reference, tests::PixelmatchIdentityParams());
    EXPECT_EQ(document_->source(), source_);
  }

  void sample(double time, std::string_view reference) {
    document_->setTime(time);
    compare(reference);
    EXPECT_DOUBLE_EQ(document_->currentTime(), time);
  }

  std::shared_ptr<const FramePresentation> captureFrame(std::uint64_t id) {
    const auto access = document_->writeAccess();
    const PresentationIdentity identity{.captureId = id, .documentGeneration = 1};
    const auto capture = CapturedPresentation::Capture(
        *document_, identity, {}, {}, {}, {}, std::nullopt, std::nullopt, std::nullopt, true);
    FramePresentationInput input;
    input.frameId = id;
    input.documentIdentity = identity;
    input.viewport.documentViewBox = Box2d::FromXYWH(0, 0, 16, 16);
    input.viewport.paneSize = Vector2d(32, 32);
    input.viewport.zoom = 2;
    input.paneClipRect = Box2d::FromXYWH(0, 0, 32, 32);
    const auto result =
        FramePresentation::Build(FramePresentationTestAccess::resources(capture, {}), input);
    EXPECT_EQ(result.failure, FramePresentationFailure::None);
    return result.frame;
  }

  std::optional<svg::SVGDocument> document_;
  std::string source_;
};

TEST_F(SvgSuiteAnimation, AbsoluteTimeInterpolatesFreezesRemovesAndResets) {
  ASSERT_NO_FATAL_FAILURE(load("absolute-time"));
  const auto frozen = document_->querySelector("#freeze");
  const auto removed = document_->querySelector("#remove");
  ASSERT_THAT(frozen, Ne(std::nullopt));
  ASSERT_THAT(removed, Ne(std::nullopt));
  sample(0, "absolute-time");
  sample(1, "absolute-mid");
  EXPECT_DOUBLE_EQ(frozen->cast<svg::SVGRectElement>().computedX().value, 6);
  EXPECT_DOUBLE_EQ(removed->cast<svg::SVGRectElement>().computedX().value, 6);
  sample(2, "absolute-end");
  sample(3, "absolute-end");
  EXPECT_DOUBLE_EQ(frozen->cast<svg::SVGRectElement>().computedX().value, 10);
  EXPECT_DOUBLE_EQ(removed->cast<svg::SVGRectElement>().computedX().value, 2);
  sample(0.5, "absolute-seek");
  document_->restartAnimations();
  EXPECT_DOUBLE_EQ(document_->currentTime(), 0);
  compare("absolute-time");
  EXPECT_THAT(document_->querySelector("#freeze"), Optional(*frozen));
}

TEST_F(SvgSuiteAnimation, TargetClickHasBoundedLifecycleAndFreshRestartNeverSession) {
  ASSERT_NO_FATAL_FAILURE(load("click-start"));
  const auto trigger = document_->querySelector("#trigger");
  const auto response = document_->querySelector("#response");
  const auto animation = document_->querySelector("#motion");
  ASSERT_THAT(trigger, Ne(std::nullopt));
  ASSERT_THAT(response, Ne(std::nullopt));
  ASSERT_THAT(animation, Ne(std::nullopt));
  sample(5, "click-start");
  EXPECT_THAT(document_->animationEvents(0, 5).events, IsEmpty());
  EXPECT_THAT(document_->dispatchMouseEvent(svg::EventType::Click, *response), IsFalse());
  compare("click-start");
  EXPECT_THAT(document_->dispatchMouseEvent(svg::EventType::Click, *trigger), IsTrue());
  sample(6, "click-mid");
  const auto batch = document_->animationEvents(4, 9);
  EXPECT_THAT(batch.complete, IsTrue());
  EXPECT_THAT(batch.events,
              ElementsAre(AllOf(Field(&Event::animation, *animation),
                                Field(&Event::type, Event::Type::Begin), Field(&Event::time, 5)),
                          AllOf(Field(&Event::type, Event::Type::Repeat),
                                Field(&Event::repeatIndex, 1), Field(&Event::time, 7)),
                          AllOf(Field(&Event::type, Event::Type::End), Field(&Event::time, 9))));
  EXPECT_DOUBLE_EQ(document_->currentTime(), 6);
  EXPECT_THAT(document_->animationEvents(5, 7).events,
              ElementsAre(AllOf(Field(&Event::type, Event::Type::Repeat), Field(&Event::time, 7))));
  sample(9, "click-end");
  document_->setTime(10);
  document_->dispatchMouseEvent(svg::EventType::Click, *trigger);
  sample(11, "click-end");
  EXPECT_THAT(document_->animationEvents(9, 12).events, IsEmpty());
  for (const double end : {10.0, std::numeric_limits<double>::infinity()}) {
    const auto invalid = document_->animationEvents(11, end);
    EXPECT_THAT(invalid.complete, IsFalse());
    EXPECT_THAT(invalid.events, IsEmpty());
    EXPECT_DOUBLE_EQ(document_->currentTime(), 11);
  }
  document_->restartAnimations();
  compare("click-start");
  EXPECT_THAT(document_->animationEvents(-1, 1).events, IsEmpty());
  EXPECT_THAT(document_->dispatchMouseEvent(svg::EventType::Click, *trigger), IsTrue());
  sample(1, "click-mid");
  ASSERT_NO_FATAL_FAILURE(load("click-start", svg::ProcessingMode::SecureAnimated));
  const auto secureTrigger = document_->querySelector("#trigger");
  ASSERT_THAT(secureTrigger, Ne(std::nullopt));
  EXPECT_THAT(document_->dispatchMouseEvent(svg::EventType::Click, *secureTrigger), IsFalse());
  sample(1, "click-start");
  ASSERT_NO_FATAL_FAILURE(load("lifecycle-bounds"));
  sample(1, "hover-hit");
  const auto excessive = document_->animationEvents(-1, 100);
  EXPECT_THAT(excessive.complete, IsFalse());
  EXPECT_THAT(excessive.events, IsEmpty());
  EXPECT_DOUBLE_EQ(document_->currentTime(), 1);
  compare("hover-hit");
}

TEST_F(SvgSuiteAnimation, HoverUsesRetainedRenderedGeometryInsteadOfLiveTargetLookup) {
  ASSERT_NO_FATAL_FAILURE(load("hover-hit"));
  const auto moving = document_->querySelector("#moving");
  ASSERT_THAT(moving, Ne(std::nullopt));
  sample(0, "hover-hit");
  const auto oldFrame = captureFrame(1);
  ASSERT_THAT(oldFrame, Ne(nullptr));
  const Vector2d oldPoint(6, 6);
  const Vector2d newPoint(22, 6);
  EXPECT_THAT(oldFrame->pointerTargetAt(oldPoint),
              Optional(moving->cast<svg::SVGGraphicsElement>()));
  EXPECT_THAT(oldFrame->pointerTargetAt(newPoint), testing::Eq(std::nullopt));
  sample(2, "hover-end");
  const auto newFrame = captureFrame(2);
  ASSERT_THAT(newFrame, Ne(nullptr));
  EXPECT_THAT(oldFrame->pointerTargetAt(oldPoint),
              Optional(moving->cast<svg::SVGGraphicsElement>()));
  EXPECT_THAT(newFrame->pointerTargetAt(oldPoint), testing::Eq(std::nullopt));
  const auto target = newFrame->pointerTargetAt(newPoint);
  ASSERT_THAT(target, Optional(moving->cast<svg::SVGGraphicsElement>()));
  EXPECT_THAT(document_->dispatchMouseEvent(svg::EventType::MouseOver, *target), IsTrue());
  compare("hover-active");
  document_->setTime(3);
  EXPECT_THAT(document_->dispatchMouseEvent(svg::EventType::MouseOut, *target), IsTrue());
  compare("hover-end");
}

}  // namespace
}  // namespace donner::editor
