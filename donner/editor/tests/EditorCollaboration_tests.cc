#include "donner/editor/EditorCollaboration.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace donner::editor {
namespace {
using Json = nlohmann::json;
using testing::Eq;
using testing::HasSubstr;

class EditorCollaborationTest : public testing::Test {
protected:
  EditorApp app;
  EditorCollaboration controller{app, {.flush = [this]() { return app.flushFrame(); }}};

  void SetUp() override {
    ASSERT_THAT(
        app.loadFromString(R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
      <g id="layer"><rect id="box" width="80" height="80" fill="red"/></g>
    </svg>)"),
        Eq(true));
  }
  Json call(std::string_view name, Json args = Json::object()) {
    return controller.handleRequest({{"jsonrpc", "2.0"},
                                     {"id", 1},
                                     {"method", "tools/call"},
                                     {"params", {{"name", name}, {"arguments", args}}}})["result"];
  }
  Json body(const Json& result) {
    return Json::parse(result["content"][0]["text"].get<std::string>());
  }
  Json revision() {
    Json state = body(call("get_editor_state"));
    return {{"document_generation", state["document_generation"]},
            {"source_revision", state["source_revision"]}};
  }
  std::string source() { return body(call("get_svg_source"))["source"].get<std::string>(); }
};

TEST_F(EditorCollaborationTest, UpdatesTheAttachedDocumentAndRecordsUndo) {
  const std::string before = source();
  Json args = revision();
  args["edits"] = {{{"selector", "#box"}, {"attribute", "fill"}, {"value", "blue"}}};
  EXPECT_THAT(call("apply_edits", args)["isError"], Eq(false));
  EXPECT_THAT(source(), HasSubstr("fill=\"blue\""));
  EXPECT_THAT(app.canUndo(), Eq(true));
  EXPECT_THAT(call("undo", revision())["isError"], Eq(false));
  EXPECT_THAT(source(), Eq(before));
  EXPECT_THAT(call("redo", revision())["isError"], Eq(false));
  EXPECT_THAT(source(), HasSubstr("fill=\"blue\""));
}

TEST_F(EditorCollaborationTest, RejectsStaleRevisionWithoutOverwritingChanges) {
  Json args = revision();
  args["edits"] = {{{"selector", "#box"}, {"attribute", "fill"}, {"value", "blue"}}};
  ASSERT_THAT(call("apply_edits", args)["isError"], Eq(false));
  args["edits"][0]["value"] = "green";
  EXPECT_THAT(call("apply_edits", args)["isError"], Eq(true));
  EXPECT_THAT(source(), HasSubstr("fill=\"blue\""));
}

TEST_F(EditorCollaborationTest, ValidatesTheEntireBatchBeforeMutating) {
  const std::string before = source();
  Json args = revision();
  args["edits"] = {{{"selector", "#box"}, {"attribute", "fill"}, {"value", "blue"}},
                   {{"selector", "#absent"}, {"attribute", "fill"}, {"value", "green"}}};
  EXPECT_THAT(call("apply_edits", args)["isError"], Eq(true));
  EXPECT_THAT(source(), Eq(before));
  EXPECT_THAT(app.canUndo(), Eq(false));
}

TEST_F(EditorCollaborationTest, InsertsIntoTheLiveDomAndDeletesWithUndo) {
  Json args = revision();
  args.update({{"parent", "#layer"},
               {"tag", "path"},
               {"attributes", {{"id", "triangle"}, {"d", "M0 0L20 0L0 20Z"}, {"fill", "blue"}}}});
  EXPECT_THAT(call("insert_element", args)["isError"], Eq(false));
  EXPECT_THAT(source(), HasSubstr("id=\"triangle\""));
  args = revision();
  args["selector"] = "#triangle";
  EXPECT_THAT(call("delete_element", args)["isError"], Eq(false));
  EXPECT_THAT(source().find("id=\"triangle\""), Eq(std::string::npos));
  EXPECT_THAT(call("undo", revision())["isError"], Eq(false));
  EXPECT_THAT(source(), HasSubstr("id=\"triangle\""));
}

TEST_F(EditorCollaborationTest, RejectsScriptsAndExternalResourceWrites) {
  for (const auto& edit :
       Json::array({{{"attribute", "onclick"}, {"value", "alert(1)"}},
                    {{"attribute", "href"}, {"value", "file:///secret"}},
                    {{"attribute", "fill"}, {"value", "url(https://example.com/paint)"}},
                    {{"attribute", "id"}, {"value", "renamed"}}})) {
    const std::string before = source();
    Json args = revision();
    Json value = edit;
    value["selector"] = "#box";
    args["edits"] = Json::array({value});
    EXPECT_THAT(call("apply_edits", args)["isError"], Eq(true));
    EXPECT_THAT(source(), Eq(before));
  }
}

TEST_F(EditorCollaborationTest, PicksAndSelectsTheSameVisibleDocument) {
  const auto picked = body(call("pick_at", {{"x", 20}, {"y", 20}}));
  EXPECT_THAT(picked["element"]["id"], Eq("box"));
  EXPECT_THAT(call("select_by_selector", {{"selector", "#box"}})["isError"], Eq(false));
  EXPECT_THAT(body(call("get_editor_state"))["selection"][0]["id"], Eq("box"));
}

TEST_F(EditorCollaborationTest, CommentsDoNotAlterSvgAndCanBeResolved) {
  const std::string before = source();
  std::string error;
  EXPECT_THAT(controller.addComment(Vector2d(10, 20), std::nullopt, "Sharpen this corner", &error),
              Eq(true));
  auto feedback = body(call("get_comments"));
  EXPECT_THAT(feedback["comments"].size(), Eq(1u));
  EXPECT_THAT(feedback["comments"][0]["text"], Eq("Sharpen this corner"));
  EXPECT_THAT(source(), Eq(before));
  EXPECT_THAT(
      call("resolve_comment", {{"comment_id", std::uint64_t(1)}, {"resolved", true}})["isError"],
      Eq(false));
  EXPECT_THAT(body(call("get_comments"))["comments"][0]["resolved"], Eq(true));
}

TEST_F(EditorCollaborationTest, CommentAnchorsFollowTransformsAndSurviveUndo) {
  std::optional<svg::SVGElement> element;
  {
    auto& doc = app.document().document();
    auto access = doc.writeAccess();
    element = doc.querySelector("#box");
  }
  std::string error;
  ASSERT_THAT(controller.addComment(Vector2d(10, 20), element, "This face", &error), Eq(true));
  Json args = revision();
  args["edits"] = {{{"selector", "#box"}, {"attribute", "transform"}, {"value", "translate(5 7)"}}};
  ASSERT_THAT(call("apply_edits", args)["isError"], Eq(false));
  controller.refreshCommentAnchors();
  auto comments = body(call("get_comments"))["comments"];
  ASSERT_THAT(comments.size(), Eq(1u));
  EXPECT_THAT(comments[0]["x"], Eq(15.0));
  EXPECT_THAT(comments[0]["y"], Eq(27.0));
  ASSERT_THAT(call("undo", revision())["isError"], Eq(false));
  controller.refreshCommentAnchors();
  comments = body(call("get_comments"))["comments"];
  ASSERT_THAT(comments.size(), Eq(1u));
  EXPECT_THAT(comments[0]["x"], Eq(10.0));
  EXPECT_THAT(comments[0]["y"], Eq(20.0));
}

TEST_F(EditorCollaborationTest, DeletedAnchorsAreReportedWithoutRetargeting) {
  std::optional<svg::SVGElement> element;
  {
    auto& doc = app.document().document();
    auto access = doc.writeAccess();
    element = doc.querySelector("#box");
  }
  std::string error;
  ASSERT_THAT(controller.addComment(Vector2d(10, 20), element, "Keep this face", &error), Eq(true));
  Json args = revision();
  args["selector"] = "#box";
  ASSERT_THAT(call("delete_element", args)["isError"], Eq(false));
  controller.refreshCommentAnchors();
  const auto comments = body(call("get_comments"))["comments"];
  ASSERT_THAT(comments.size(), Eq(1u));
  EXPECT_THAT(comments[0]["orphaned"], Eq(true));
  EXPECT_THAT(comments[0]["element_id"], Eq("box"));
}

TEST_F(EditorCollaborationTest, FeedbackCheckpointPreservesIdsAndDocumentScope) {
  app.setCurrentFilePath("art.svg");
  std::string error;
  ASSERT_THAT(controller.addComment(Vector2d(10, 20), std::nullopt, "Retain this note", &error),
              Eq(true));
  const Json archive = controller.feedbackArchive();
  EditorCollaboration restored(app, {});
  ASSERT_THAT(restored.restoreFeedback(archive), Eq(true));
  EXPECT_THAT(restored.feedback()["comments"][0]["id"], Eq(1));
  app.setCurrentFilePath("another.svg");
  EXPECT_THAT(restored.feedback()["comments"].empty(), Eq(true));
  app.setCurrentFilePath("art.svg");
  EXPECT_THAT(restored.feedback()["comments"].size(), Eq(1u));
  Json invalid = archive;
  invalid["comments"].push_back(invalid["comments"][0]);
  EXPECT_THAT(restored.restoreFeedback(invalid), Eq(false));
  EXPECT_THAT(restored.feedback()["comments"].size(), Eq(1u));
}

TEST_F(EditorCollaborationTest, RejectsOversizedCommentsAndMixedExternalPaintReferences) {
  std::string error;
  EXPECT_THAT(controller.addComment(Vector2d(1, 2), std::nullopt, std::string(4097, 'x'), &error),
              Eq(false));
  Json args = revision();
  args["edits"] = {{{"selector", "#box"},
                    {"attribute", "fill"},
                    {"value", "URL(#safe) url(https://example.com/unsafe)"}}};
  EXPECT_THAT(call("apply_edits", args)["isError"], Eq(true));
}

TEST_F(EditorCollaborationTest, RejectsMalformedRequestsWithoutChangingTheDocument) {
  const std::string before = source();
  EXPECT_THAT(controller.handleRequest(Json::array()).contains("error"), Eq(true));
  EXPECT_THAT(controller.handleRequest({{"id", 1}, {"method", "tools/call"}, {"params", "invalid"}})
                  .contains("error"),
              Eq(true));
  EXPECT_THAT(source(), Eq(before));
}
}  // namespace
}  // namespace donner::editor
