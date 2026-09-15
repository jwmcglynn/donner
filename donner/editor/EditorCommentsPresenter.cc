#include "donner/editor/EditorCommentsPresenter.h"

#include "donner/editor/ImGuiIncludes.h"

namespace donner::editor {
void EditorCommentsPresenter::beginComment(EditorCollaboration& collaboration, Vector2d point,
                                           std::optional<svg::SVGElement> element) {
  pendingAnchor_ = collaboration.captureCommentAnchor(point, element);
  visible_ = true;
  focusComposer_ = true;
  draft_.fill(0);
  error_.clear();
}

void EditorCommentsPresenter::drawPins(EditorCollaboration& collaboration,
                                       const ViewportState& viewport) {
  const ImVec2 oldCursor = ImGui::GetCursorScreenPos();
  pinRegions_.clear();
  drawPendingAnchor(collaboration, viewport);
  for (const auto& comment : collaboration.comments()) {
    if (comment.resolved || !collaboration.isCurrentComment(comment)) continue;
    const Vector2d point = viewport.documentToScreen(comment.presentedPoint);
    if (point.x < viewport.paneOrigin.x || point.y < viewport.paneOrigin.y ||
        point.x >= viewport.paneOrigin.x + viewport.paneSize.x ||
        point.y >= viewport.paneOrigin.y + viewport.paneSize.y)
      continue;
    const std::string label = std::to_string(comment.id);
    ImGui::PushID(label.c_str());
    ImGui::SetCursorScreenPos(
        ImVec2(static_cast<float>(point.x - 10), static_cast<float>(point.y - 10)));
    pinRegions_.push_back(Box2d::FromXYWH(point.x - 10, point.y - 10, 22, 22));
    if (ImGui::Button(label.c_str(), ImVec2(22, 22))) visible_ = true;
    if (ImGui::IsItemHovered()) {
      ImGui::BeginTooltip();
      ImGui::TextUnformatted(comment.text.c_str());
      ImGui::EndTooltip();
    }
    ImGui::PopID();
  }
  ImGui::SetCursorScreenPos(oldCursor);
}

void EditorCommentsPresenter::drawPendingAnchor(EditorCollaboration& collaboration,
                                                const ViewportState& viewport) {
  if (!visible_ || !pendingAnchor_ || !collaboration.isCurrentComment(*pendingAnchor_)) return;
  const Vector2d point = viewport.documentToScreen(pendingAnchor_->presentedPoint);
  ImDrawList* drawList = ImGui::GetWindowDrawList();
  drawList->PushClipRect(
      ImVec2(static_cast<float>(viewport.paneOrigin.x), static_cast<float>(viewport.paneOrigin.y)),
      ImVec2(static_cast<float>(viewport.paneOrigin.x + viewport.paneSize.x),
             static_cast<float>(viewport.paneOrigin.y + viewport.paneSize.y)),
      true);
  const ImVec2 center(static_cast<float>(point.x), static_cast<float>(point.y));
  drawList->AddCircle(center, 11.0f, IM_COL32(20, 28, 40, 240), 32, 5.0f);
  drawList->AddCircle(center, 11.0f, IM_COL32(114, 202, 255, 255), 32, 2.0f);
  drawList->AddCircleFilled(center, 3.0f, IM_COL32(114, 202, 255, 255));
  drawList->PopClipRect();
}

void EditorCommentsPresenter::drawPanel(EditorCollaboration& collaboration, bool rendererIdle,
                                        const Box2d& initialBounds) {
  if (!visible_) return;
  ImGui::SetNextWindowPos(ImVec2(static_cast<float>(initialBounds.topLeft.x),
                                 static_cast<float>(initialBounds.topLeft.y)),
                          ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(
      ImVec2(static_cast<float>(initialBounds.width()), static_cast<float>(initialBounds.height())),
      ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Comments", &visible_)) {
    ImGui::End();
    return;
  }
  ImGui::TextWrapped("Right-click the artwork and choose Add Comment Here.");
  if (!persistenceError_.empty()) ImGui::TextWrapped("%s", persistenceError_.c_str());
  drawComposer(collaboration, rendererIdle);
  drawCommentList(collaboration);
  ImGui::End();
}
void EditorCommentsPresenter::drawComposer(EditorCollaboration& collaboration, bool rendererIdle) {
  if (!pendingAnchor_) return;

  ImGui::Separator();
  ImGui::Text("New comment at %.1f, %.1f", pendingAnchor_->documentPoint.x,
              pendingAnchor_->documentPoint.y);
  if (focusComposer_) {
    ImGui::SetKeyboardFocusHere();
    focusComposer_ = false;
  }
  ImGui::InputTextMultiline("##comment_text", draft_.data(), draft_.size(), ImVec2(-1, 110));
  ImGui::BeginDisabled(!rendererIdle || draft_[0] == 0);
  if (ImGui::Button("Add Comment")) {
    if (collaboration.addAnchoredComment(*pendingAnchor_, draft_.data(), &error_)) {
      pendingAnchor_.reset();
      draft_.fill(0);
    }
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) {
    pendingAnchor_.reset();
    error_.clear();
  }
  if (!error_.empty()) ImGui::TextWrapped("%s", error_.c_str());
}
void EditorCommentsPresenter::drawCommentList(EditorCollaboration& collaboration) {
  ImGui::Separator();
  bool any = false;
  for (const auto& comment : collaboration.comments()) {
    if (!collaboration.isCurrentComment(comment)) continue;
    any = true;
    const std::string id = std::to_string(comment.id);
    ImGui::PushID(id.c_str());
    ImGui::Text("%s  %s", id.c_str(),
                comment.elementLabel.empty() ? "Canvas" : comment.elementLabel.c_str());
    ImGui::TextWrapped("%s", comment.text.c_str());
    if (comment.orphaned) ImGui::TextDisabled("The anchored element was removed.");
    bool resolved = comment.resolved;
    if (ImGui::Checkbox("Resolved", &resolved))
      collaboration.setCommentResolved(comment.id, resolved);
    ImGui::Separator();
    ImGui::PopID();
  }
  if (!any) ImGui::TextDisabled("No comments on this document yet.");
}

bool EditorCommentsPresenter::capturesInput(Vector2d point) const {
  for (const auto& region : pinRegions_)
    if (region.contains(point)) return true;
  return false;
}
}  // namespace donner::editor
