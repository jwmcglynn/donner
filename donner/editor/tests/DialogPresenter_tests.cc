#include "donner/editor/DialogPresenter.h"

#include <gtest/gtest.h>

#include <string>

#include "donner/editor/ImGuiIncludes.h"

namespace donner::editor {
namespace {

class DialogPresenterTest : public ::testing::Test {
protected:
  void SetUp() override {
    IMGUI_CHECKVERSION();
    context_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.Fonts->Build();
  }

  void TearDown() override {
    ImGui::DestroyContext(context_);
    context_ = nullptr;
  }

  static void RenderFrame(DialogPresenter& presenter, int* openCalls, int* saveCalls) {
    ImGui::NewFrame();
    presenter.render(
        [openCalls](std::string_view, std::string* error) {
          ++*openCalls;
          *error = "open failed";
          return false;
        },
        [saveCalls](std::string_view, std::string* error) {
          ++*saveCalls;
          *error = "save failed";
          return false;
        });
    ImGui::Render();
  }

private:
  ImGuiContext* context_ = nullptr;
};

TEST_F(DialogPresenterTest, RendersRequestedOpenSaveAndAboutDialogs) {
  DialogPresenter presenter("third-party notices");
  presenter.requestOpenFile(std::string("/tmp/current.svg"));
  presenter.requestSaveFile(std::string("/tmp/save.svg"), "previous save error");
  presenter.requestAbout();
  presenter.setOpenFileError("previous open error");

  int openCalls = 0;
  int saveCalls = 0;
  RenderFrame(presenter, &openCalls, &saveCalls);

  EXPECT_EQ(openCalls, 0);
  EXPECT_EQ(saveCalls, 0);

  presenter.clearOpenFileError();
  presenter.clearSaveFileError();
  RenderFrame(presenter, &openCalls, &saveCalls);

  EXPECT_EQ(openCalls, 0);
  EXPECT_EQ(saveCalls, 0);
}

TEST_F(DialogPresenterTest, RequestsWithoutCurrentPathRenderEmptyPathBuffers) {
  DialogPresenter presenter("");
  presenter.requestOpenFile(std::nullopt);
  presenter.requestSaveFile(std::nullopt);

  int openCalls = 0;
  int saveCalls = 0;
  RenderFrame(presenter, &openCalls, &saveCalls);

  EXPECT_EQ(openCalls, 0);
  EXPECT_EQ(saveCalls, 0);
}

}  // namespace
}  // namespace donner::editor
