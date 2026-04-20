#include "donner/editor/sandbox/EditorBackendCore.h"

#include "donner/editor/sandbox/SerializingRenderer.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGeometryElement.h"

namespace donner::editor::sandbox {

EditorBackendCore::EditorBackendCore() = default;
EditorBackendCore::~EditorBackendCore() = default;

FramePayload EditorBackendCore::buildFramePayload() {
  FramePayload frame;
  frame.frameId = frameIdCounter_++;

  if (editor_.hasDocument()) {
    editor_.flushFrame();

    SerializingRenderer renderer;
    auto& doc = editor_.document().document();
    doc.setCanvasSize(viewportWidth_, viewportHeight_);
    renderer.draw(doc);

    frame.statusKind =
        renderer.hasUnsupported() ? FrameStatusKind::kRenderedLossy : FrameStatusKind::kRendered;
    frame.renderWire = std::move(renderer).takeBuffer();

    // Populate selection entries.
    for (const auto& elem : editor_.selectedElements()) {
      auto geometry = elem.tryCast<donner::svg::SVGGeometryElement>();
      if (!geometry.has_value()) {
        continue;
      }

      auto bounds = geometry->worldBounds();
      if (!bounds.has_value()) {
        continue;
      }

      FrameSelectionEntry entry;
      entry.bbox[0] = bounds->topLeft.x;
      entry.bbox[1] = bounds->topLeft.y;
      entry.bbox[2] = bounds->bottomRight.x;
      entry.bbox[3] = bounds->bottomRight.y;
      entry.hasTransform = false;
      entry.handleMask = 0xFF;
      frame.selections.push_back(entry);
    }

    // Drain writebacks to prevent unbounded growth.
    (void)editor_.consumeTransformWriteback();
    (void)editor_.consumeElementRemoveWritebacks();
  } else {
    frame.statusKind = FrameStatusKind::kNone;
  }

  return frame;
}

FramePayload EditorBackendCore::handleSetViewport(const SetViewportPayload& vp) {
  if (vp.width > 0 && vp.height > 0) {
    viewportWidth_ = vp.width;
    viewportHeight_ = vp.height;
  }
  return buildFramePayload();
}

FramePayload EditorBackendCore::handleLoadBytes(const LoadBytesPayload& load) {
  (void)editor_.loadFromString(load.bytes);
  editor_.setCleanSourceText(load.bytes);
  return buildFramePayload();
}

FramePayload EditorBackendCore::handleReplaceSource(const ReplaceSourcePayload& rep) {
  (void)editor_.loadFromString(rep.bytes);
  return buildFramePayload();
}

FramePayload EditorBackendCore::handleApplySourcePatch(const ApplySourcePatchPayload& /*patch*/) {
  return buildFramePayload();
}

FramePayload EditorBackendCore::handlePointerEvent(const PointerEventPayload& ptr) {
  if (ptr.phase == PointerPhase::kDown && editor_.hasDocument()) {
    auto hit = editor_.hitTest(Vector2d(ptr.documentX, ptr.documentY));
    if (hit.has_value()) {
      editor_.setSelection(donner::svg::SVGElement(*hit));
    } else {
      editor_.clearSelection();
    }
  }
  return buildFramePayload();
}

FramePayload EditorBackendCore::handleKeyEvent(const KeyEventPayload& /*key*/) {
  return buildFramePayload();
}

FramePayload EditorBackendCore::handleWheelEvent(const WheelEventPayload& /*wheel*/) {
  return buildFramePayload();
}

FramePayload EditorBackendCore::handleSetTool(const SetToolPayload& /*tool*/) {
  return buildFramePayload();
}

FramePayload EditorBackendCore::handleUndo() {
  editor_.undo();
  return buildFramePayload();
}

FramePayload EditorBackendCore::handleRedo() {
  editor_.redo();
  return buildFramePayload();
}

ExportResponsePayload EditorBackendCore::handleExport(const ExportRequestPayload& req) {
  ExportResponsePayload resp;
  resp.format = req.format;
  // Placeholder: empty bytes.
  return resp;
}

}  // namespace donner::editor::sandbox
