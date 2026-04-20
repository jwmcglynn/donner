#include "donner/editor/sandbox/EditorBackendCore.h"

#include <algorithm>

#include "donner/base/xml/XMLNode.h"
#include "donner/editor/sandbox/SerializingRenderer.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGGeometryElement.h"

namespace donner::editor::sandbox {

EditorBackendCore::EditorBackendCore() = default;
EditorBackendCore::~EditorBackendCore() = default;

void EditorBackendCore::bumpEntityGeneration() {
  ++entityGeneration_;
  entityToId_.clear();
  idToElement_.clear();
  nextEntityId_ = 1;
}

uint64_t EditorBackendCore::entityIdFor(entt::entity entity, const svg::SVGElement& element) {
  auto it = entityToId_.find(entity);
  if (it != entityToId_.end()) {
    return it->second;
  }
  uint64_t id = nextEntityId_++;
  entityToId_[entity] = id;
  idToElement_.emplace(id, element);
  return id;
}

std::optional<svg::SVGElement> EditorBackendCore::resolveElement(
    uint64_t entityId, uint64_t entityGeneration) const {
  if (entityGeneration != entityGeneration_) {
    return std::nullopt;
  }
  auto it = idToElement_.find(entityId);
  if (it == idToElement_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void EditorBackendCore::populateTreeSummary(FrameTreeSummary& tree) {
  tree.generation = entityGeneration_;
  tree.nodes.clear();
  tree.rootIndex = 0xFFFFFFFF;

  if (!editor_.hasDocument()) {
    return;
  }

  auto& doc = editor_.document().document();
  svg::SVGSVGElement root = doc.svgElement();
  const auto& selectedElems = editor_.selectedElements();

  // Depth-first pre-order traversal with explicit stack.
  struct StackEntry {
    svg::SVGElement element;
    uint32_t parentIndex;
    uint32_t depth;
  };
  std::vector<StackEntry> stack;
  // SVGSVGElement IS-A SVGElement; copy-construct to upcast.
  stack.push_back({root, 0xFFFFFFFF, 0});

  while (!stack.empty()) {
    auto [element, parentIdx, depth] = std::move(stack.back());
    stack.pop_back();

    uint32_t nodeIndex = static_cast<uint32_t>(tree.nodes.size());

    TreeNodeEntry entry;
    entry.entityId = entityIdFor(element.entityHandle().entity(), element);
    entry.entityGeneration = entityGeneration_;
    entry.parentIndex = parentIdx;
    entry.depth = depth;

    // Tag name.
    auto tagNameRef = element.tagName();
    entry.tagName = std::string(tagNameRef.name);

    // id attribute.
    RcString elemId = element.id();
    if (!elemId.empty()) {
      entry.idAttr = std::string(std::string_view(elemId));
    }

    // Display name: "<tag>" + optional ' id="..."' + optional ' class="..."'.
    entry.displayName = "<" + entry.tagName;
    if (!entry.idAttr.empty()) {
      entry.displayName += " id=\"" + entry.idAttr + "\"";
    }
    RcString cls = element.className();
    if (!cls.empty()) {
      entry.displayName += " class=\"" + std::string(std::string_view(cls)) + "\"";
    }
    entry.displayName += ">";

    // Source range.
    auto xmlNode = xml::XMLNode::TryCast(element.entityHandle());
    if (xmlNode.has_value()) {
      auto loc = xmlNode->getNodeLocation();
      if (loc.has_value()) {
        entry.sourceStart =
            static_cast<uint32_t>(loc->start.offset.value_or(0));
        entry.sourceEnd = static_cast<uint32_t>(loc->end.offset.value_or(0));
      }
    }

    // Selected?
    entry.selected =
        std::find_if(selectedElems.begin(), selectedElems.end(),
                     [&](const svg::SVGElement& sel) {
                       return sel.entityHandle().entity() ==
                              element.entityHandle().entity();
                     }) != selectedElems.end();

    tree.nodes.push_back(std::move(entry));

    if (parentIdx == 0xFFFFFFFF) {
      tree.rootIndex = nodeIndex;
    }

    // Push children in reverse order so first child is processed first.
    std::vector<svg::SVGElement> children;
    for (auto child = element.firstChild(); child.has_value();
         child = child->nextSibling()) {
      children.push_back(*child);
    }
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
      stack.push_back({*it, nodeIndex, depth + 1});
    }
  }
}

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

    // Tree summary.
    populateTreeSummary(frame.tree);
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
  bumpEntityGeneration();
  (void)editor_.loadFromString(load.bytes);
  editor_.setCleanSourceText(load.bytes);
  return buildFramePayload();
}

FramePayload EditorBackendCore::handleReplaceSource(const ReplaceSourcePayload& rep) {
  bumpEntityGeneration();
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

FramePayload EditorBackendCore::handleSelectElement(const SelectElementPayload& sel) {
  auto element = resolveElement(sel.entityId, sel.entityGeneration);
  if (element.has_value() && editor_.hasDocument()) {
    switch (sel.mode) {
      case 0:  // Replace
        editor_.setSelection(*element);
        break;
      case 1:  // Toggle
        editor_.toggleInSelection(*element);
        break;
      case 2:  // Add
        editor_.addToSelection(*element);
        break;
      default:
        break;
    }
  }
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
