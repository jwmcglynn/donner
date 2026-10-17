#define main CodeSizeExecutableMain
#include "code_size/MinimalMain.cc"
#undef main

#include <gtest/gtest.h>

#include "harness/NullRecordingRenderer.h"

namespace {

class EmptyBackend final : public donner::bakeoff::BenchBackend {
public:
  explicit EmptyBackend(bool completes) : completes_(completes) {}
  donner::svg::RendererInterface& renderer() override { return renderer_; }
  bool finishGpuWork() override { return completes_; }

private:
  bool completes_;
  donner::bakeoff::NullRecordingRenderer renderer_;
};

donner::bakeoff::BackendInfo Backend(donner::bakeoff::Suite suite, bool completes) {
  donner::bakeoff::BackendInfo info;
  info.name = "test";
  info.suite = suite;
  info.create = [completes] { return std::make_unique<EmptyBackend>(completes); };
  return info;
}

TEST(CodeSizeSmokeTest, ReferenceAllowsEmptyTraversalSnapshot) {
  EXPECT_EQ(RunCodeSize(Backend(donner::bakeoff::Suite::Reference, true)), 0);
}

TEST(CodeSizeSmokeTest, RenderingSuitesRejectEmptySnapshot) {
  EXPECT_EQ(RunCodeSize(Backend(donner::bakeoff::Suite::Software, true)), 6);
  EXPECT_EQ(RunCodeSize(Backend(donner::bakeoff::Suite::Gpu, true)), 6);
}

TEST(CodeSizeSmokeTest, CompletionFailureFailsEvenForReference) {
  EXPECT_EQ(RunCodeSize(Backend(donner::bakeoff::Suite::Gpu, false)), 5);
  EXPECT_EQ(RunCodeSize(Backend(donner::bakeoff::Suite::Reference, false)), 5);
}

}  // namespace
