#include "donner/editor/SamplePickerController.h"

#include <gtest/gtest.h>

#include <optional>
#include <string_view>

namespace donner::editor {
namespace {

TEST(SamplePickerController, DonnerRepositoryUsesFixedHttpsDestination) {
  EXPECT_EQ(ExternalUrlValue(ExternalUrlTarget::DonnerRepository),
            std::string_view("https://github.com/jwmcglynn/donner"));
}

TEST(SamplePickerController, OpensTypedRepositoryDestination) {
  std::optional<ExternalUrlTarget> launchedTarget;
  SamplePickerController controller([&launchedTarget](ExternalUrlTarget target) {
    launchedTarget = target;
    return true;
  });
  SamplePickerActions actions;
  actions.openGitHub = true;

  EXPECT_TRUE(controller.applyExternalActions(actions));
  ASSERT_TRUE(launchedTarget.has_value());
  EXPECT_EQ(*launchedTarget, ExternalUrlTarget::DonnerRepository);
}

TEST(SamplePickerController, IgnoresFramesWithoutExternalAction) {
  int launchCount = 0;
  SamplePickerController controller([&launchCount](ExternalUrlTarget) {
    ++launchCount;
    return true;
  });

  EXPECT_FALSE(controller.applyExternalActions(SamplePickerActions{}));
  EXPECT_EQ(launchCount, 0);
}

TEST(SamplePickerController, PropagatesPlatformLaunchFailure) {
  SamplePickerController controller([](ExternalUrlTarget) { return false; });
  SamplePickerActions actions;
  actions.openGitHub = true;

  EXPECT_FALSE(controller.applyExternalActions(actions));
}

}  // namespace
}  // namespace donner::editor
