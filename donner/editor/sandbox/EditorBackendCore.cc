#include "donner/editor/sandbox/EditorBackendCore.h"

#include <algorithm>
#include <limits>
#include <numeric>
#include <span>
#include <sstream>

#include "donner/base/xml/XMLNode.h"
#include "donner/editor/OverlayRenderer.h"
#include "donner/editor/SelectionAabb.h"
#include "donner/editor/TextPatch.h"
#include "donner/editor/backend_lib/AttributeWriteback.h"
#include "donner/editor/backend_lib/EditorCommand.h"
#include "donner/editor/sandbox/EditorApiCodec.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/properties/Property.h"

namespace donner::editor::sandbox {

namespace {

/// Premul-alpha "source-over": premultiplied src over premultiplied
/// dst. `out_premul = src_premul + dst_premul * (1 - src.a)`. No
/// division — that's why the compositor caches its bitmaps in premul.
void CompositePremulOntoPremul(donner::svg::RendererBitmap& dst,
                               const donner::svg::RendererBitmap& src, int offsetX, int offsetY) {
  if (src.empty() || dst.empty()) {
    return;
  }
  const int dstW = dst.dimensions.x;
  const int dstH = dst.dimensions.y;
  const int srcW = src.dimensions.x;
  const int srcH = src.dimensions.y;
  const int xStart = std::max(0, offsetX);
  const int yStart = std::max(0, offsetY);
  const int xEnd = std::min(dstW, offsetX + srcW);
  const int yEnd = std::min(dstH, offsetY + srcH);
  if (xStart >= xEnd || yStart >= yEnd) {
    return;
  }
  for (int y = yStart; y < yEnd; ++y) {
    uint8_t* dstRow = dst.pixels.data() + static_cast<size_t>(y) * dst.rowBytes;
    const uint8_t* srcRow = src.pixels.data() + static_cast<size_t>(y - offsetY) * src.rowBytes;
    for (int x = xStart; x < xEnd; ++x) {
      const size_t dstOff = static_cast<size_t>(x) * 4;
      const size_t srcOff = static_cast<size_t>(x - offsetX) * 4;
      const uint32_t sa = srcRow[srcOff + 3];
      if (sa == 0u) continue;
      if (sa == 255u) {
        dstRow[dstOff + 0] = srcRow[srcOff + 0];
        dstRow[dstOff + 1] = srcRow[srcOff + 1];
        dstRow[dstOff + 2] = srcRow[srcOff + 2];
        dstRow[dstOff + 3] = 255u;
        continue;
      }
      const uint32_t invA = 255u - sa;
      const auto blend = [invA](uint32_t s, uint32_t d) -> uint8_t {
        return static_cast<uint8_t>(s + ((d * invA + 127u) / 255u));
      };
      dstRow[dstOff + 0] = blend(srcRow[srcOff + 0], dstRow[dstOff + 0]);
      dstRow[dstOff + 1] = blend(srcRow[srcOff + 1], dstRow[dstOff + 1]);
      dstRow[dstOff + 2] = blend(srcRow[srcOff + 2], dstRow[dstOff + 2]);
      dstRow[dstOff + 3] = blend(srcRow[srcOff + 3], dstRow[dstOff + 3]);
    }
  }
}

/// In-place unpremultiply so the final bitmap shipped over the wire
/// matches host-side straight-alpha expectations (GL's
/// `glTexImage2D(GL_RGBA, GL_UNSIGNED_BYTE, …)` is straight by default,
/// ImGui blends with `GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA`).
void UnpremultiplyInPlace(donner::svg::RendererBitmap& bitmap) {
  if (bitmap.alphaType == donner::svg::AlphaType::Unpremultiplied) return;
  const auto height = static_cast<size_t>(bitmap.dimensions.y);
  const auto width = static_cast<size_t>(bitmap.dimensions.x);
  for (size_t y = 0; y < height; ++y) {
    uint8_t* row = bitmap.pixels.data() + y * bitmap.rowBytes;
    for (size_t x = 0; x < width; ++x) {
      const uint32_t a = row[x * 4 + 3];
      if (a == 0u || a == 255u) continue;
      const uint32_t half = a >> 1u;
      row[x * 4 + 0] = static_cast<uint8_t>(std::min(255u, (row[x * 4 + 0] * 255u + half) / a));
      row[x * 4 + 1] = static_cast<uint8_t>(std::min(255u, (row[x * 4 + 1] * 255u + half) / a));
      row[x * 4 + 2] = static_cast<uint8_t>(std::min(255u, (row[x * 4 + 2] * 255u + half) / a));
    }
  }
  bitmap.alphaType = donner::svg::AlphaType::Unpremultiplied;
}

FrameBitmapPayload MakeFrameBitmapPayload(const donner::svg::RendererBitmap& bitmap) {
  FrameBitmapPayload payload;
  payload.width = bitmap.dimensions.x;
  payload.height = bitmap.dimensions.y;
  payload.rowBytes = static_cast<uint32_t>(bitmap.rowBytes);
  payload.alphaType = static_cast<uint8_t>(bitmap.alphaType);
  payload.pixels = bitmap.pixels;
  return payload;
}

std::optional<TextPatch> BuildTransformWritebackPatch(
    std::string_view source, const donner::editor::AttributeWritebackTarget& target,
    const Transform2d& transform, const std::optional<RcString>& sourceTransformAttributeValue,
    bool restoreSourceTransformAttributeValue) {
  if (restoreSourceTransformAttributeValue && sourceTransformAttributeValue.has_value()) {
    return buildAttributeWriteback(source, target, "transform",
                                   std::string_view(*sourceTransformAttributeValue));
  }

  const RcString serialized = toSVGTransformString(transform);
  if (std::string_view(serialized).empty()) {
    return buildAttributeRemoveWriteback(source, target, "transform");
  }

  return buildAttributeWriteback(source, target, "transform", std::string_view(serialized));
}

FrameWritebackEntry MakeWritebackEntry(const TextPatch& patch, WritebackReason reason) {
  FrameWritebackEntry entry;
  entry.start = static_cast<uint32_t>(patch.offset);
  entry.end = static_cast<uint32_t>(patch.offset + patch.length);
  entry.newText = patch.replacement;
  entry.reason = reason;
  return entry;
}

std::optional<std::size_t> ShiftOffset(std::size_t offset, int64_t delta) {
  if (delta < 0) {
    const auto magnitude = static_cast<std::size_t>(-delta);
    if (offset < magnitude) {
      return std::nullopt;
    }
    return offset - magnitude;
  }

  const auto magnitude = static_cast<std::size_t>(delta);
  if (offset > std::numeric_limits<std::size_t>::max() - magnitude) {
    return std::nullopt;
  }
  return offset + magnitude;
}

bool AdjustSourceRangeForPatch(std::size_t* start, std::size_t* end, const TextPatch& patch) {
  if (patch.offset > std::numeric_limits<std::size_t>::max() - patch.length) {
    return false;
  }

  const std::size_t patchStart = patch.offset;
  const std::size_t patchEnd = patch.offset + patch.length;
  const int64_t delta =
      static_cast<int64_t>(patch.replacement.size()) - static_cast<int64_t>(patch.length);

  if (*end <= patchStart) {
    return true;
  }

  if (*start >= patchEnd) {
    auto shiftedStart = ShiftOffset(*start, delta);
    auto shiftedEnd = ShiftOffset(*end, delta);
    if (!shiftedStart.has_value() || !shiftedEnd.has_value()) {
      return false;
    }
    *start = *shiftedStart;
    *end = *shiftedEnd;
    return true;
  }

  if (patchStart >= *start && patchEnd <= *end) {
    auto shiftedEnd = ShiftOffset(*end, delta);
    if (!shiftedEnd.has_value()) {
      return false;
    }
    *end = *shiftedEnd;
    return true;
  }

  return false;
}

void AdjustXmlNodeSourceRangesForPatch(xml::XMLNode node, const TextPatch& patch) {
  if (auto loc = node.getNodeLocation();
      loc.has_value() && loc->start.offset.has_value() && loc->end.offset.has_value()) {
    std::size_t start = loc->start.offset.value();
    std::size_t end = loc->end.offset.value();
    if (AdjustSourceRangeForPatch(&start, &end, patch)) {
      node.setSourceStartOffset(FileOffset::Offset(start));
      node.setSourceEndOffset(FileOffset::Offset(end));
    }
  }

  for (auto child = node.firstChild(); child.has_value(); child = child->nextSibling()) {
    AdjustXmlNodeSourceRangesForPatch(*child, patch);
  }
}

void AdjustLiveSourceRangesForPatches(EditorApp& editor, std::span<const TextPatch> patches) {
  if (!editor.hasDocument() || patches.empty()) {
    return;
  }

  auto root = xml::XMLNode::TryCast(editor.document().document().svgElement().entityHandle());
  if (!root.has_value()) {
    return;
  }

  std::vector<std::size_t> indices(patches.size());
  std::iota(indices.begin(), indices.end(), 0);
  std::sort(indices.begin(), indices.end(), [&](std::size_t lhs, std::size_t rhs) {
    return patches[lhs].offset > patches[rhs].offset;
  });

  for (const std::size_t index : indices) {
    AdjustXmlNodeSourceRangesForPatch(*root, patches[index]);
  }
}

bool IsDeleteKey(const KeyEventPayload& key) {
  // GLFW key codes. Duplicated here rather than depending on GLFW from
  // the backend core: the session protocol already defines keyCode as
  // the host-side GLFW value.
  constexpr int32_t kGlfwKeyBackspace = 259;
  constexpr int32_t kGlfwKeyDelete = 261;
  return key.phase == KeyPhase::kDown &&
         (key.keyCode == kGlfwKeyBackspace || key.keyCode == kGlfwKeyDelete);
}

std::vector<donner::editor::AttributeWritebackTarget> CaptureSelectionTargets(EditorApp& editor) {
  std::vector<donner::editor::AttributeWritebackTarget> targets;
  if (!editor.hasDocument()) {
    return targets;
  }

  targets.reserve(editor.selectedElements().size());
  for (const auto& element : editor.selectedElements()) {
    if (auto target = captureAttributeWritebackTarget(element); target.has_value()) {
      targets.push_back(std::move(*target));
    }
  }
  return targets;
}

void RestoreSelectionTargets(EditorApp& editor,
                             std::span<const donner::editor::AttributeWritebackTarget> targets) {
  if (!editor.hasDocument() || targets.empty()) {
    return;
  }

  std::vector<svg::SVGElement> restoredSelection;
  restoredSelection.reserve(targets.size());
  for (const auto& target : targets) {
    if (auto element = resolveAttributeWritebackTarget(editor.document().document(), target);
        element.has_value()) {
      restoredSelection.push_back(*element);
    }
  }
  editor.setSelection(std::move(restoredSelection));
}

}  // namespace

EditorBackendCore::EditorBackendCore() {
  // Always-on in the sandbox — the backend owns both `SelectTool` and
  // the `CompositorController`, so there's no off-by-default gate to
  // wait on (unlike main, where `EditorShell` flips this via a CLI
  // flag). With the flag enabled, `SelectTool::activeDragPreview()`
  // surfaces the current drag target so `buildFramePayload` can
  // route it into `compositor_.promoteEntity(..., ActiveDrag)`, which
  // is what engages the translation-only fast path during drag.
  selectTool_.setCompositedDragPreviewEnabled(true);
}
EditorBackendCore::~EditorBackendCore() = default;

void EditorBackendCore::bumpEntityGeneration() {
  ++entityGeneration_;
  entityToId_.clear();
  idToElement_.clear();
  nextEntityId_ = 1;
  resetCompositorState();
}

void EditorBackendCore::resetCompositorState() {
  // Layer / segment caches are snapshots of the current DOM structure.
  // Drop the whole compositor so `buildFramePayload` rebuilds split
  // static layers against the post-mutation document on the next render.
  compositor_.reset();
  compositorEntity_ = entt::null;
  compositorInteractionKind_ = donner::svg::compositor::InteractionHint::Selection;
  compositedPreviewUploadsPrimed_ = false;
  compositedPreviewUploadEntity_ = entt::null;
  compositedPreviewUploadCanvasSize_ = Vector2i(-1, -1);
  compositedPreviewUploadGeneration_ = 0;
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

std::optional<svg::SVGElement> EditorBackendCore::resolveElement(uint64_t entityId,
                                                                 uint64_t entityGeneration) const {
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
        entry.sourceStart = static_cast<uint32_t>(loc->start.offset.value_or(0));
        entry.sourceEnd = static_cast<uint32_t>(loc->end.offset.value_or(0));
      }
    }

    // Selected?
    entry.selected =
        std::find_if(selectedElems.begin(), selectedElems.end(), [&](const svg::SVGElement& sel) {
          return sel.entityHandle().entity() == element.entityHandle().entity();
        }) != selectedElems.end();

    tree.nodes.push_back(std::move(entry));

    if (parentIdx == 0xFFFFFFFF) {
      tree.rootIndex = nodeIndex;
    }

    // Push children in reverse order so first child is processed first.
    std::vector<svg::SVGElement> children;
    for (auto child = element.firstChild(); child.has_value(); child = child->nextSibling()) {
      children.push_back(*child);
    }
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
      stack.push_back({*it, nodeIndex, depth + 1});
    }
  }
}

void EditorBackendCore::populateInspectedElement(FramePayload& frame) {
  frame.hasInspectedElement = false;

  if (!editor_.hasDocument()) {
    return;
  }
  const auto& selected = editor_.selectedElements();
  // Single-selection only — the sidebar renders one element at a time.
  // Multi-selection would need a separate "properties common to all"
  // mode that we haven't designed yet.
  if (selected.size() != 1) {
    return;
  }

  const svg::SVGElement& element = selected.front();
  auto xmlNodeOpt = xml::XMLNode::TryCast(element.entityHandle());
  if (!xmlNodeOpt.has_value()) {
    return;
  }
  const xml::XMLNode& xmlNode = *xmlNodeOpt;

  auto& insp = frame.inspectedElement;
  insp.entityId = entityIdFor(element.entityHandle().entity(), element);
  insp.entityGeneration = entityGeneration_;
  auto tagNameRef = element.tagName();
  insp.tagName = std::string(tagNameRef.name);

  RcString elemId = element.id();
  insp.idAttr = elemId.empty() ? std::string() : std::string(std::string_view(elemId));
  RcString cls = element.className();
  insp.className = cls.empty() ? std::string() : std::string(std::string_view(cls));

  // XML attributes, in document order. `XMLNode::attributes()` returns
  // the authored list including any namespace prefix; we flatten with
  // a simple "prefix:name" rendering when a prefix is present.
  insp.xmlAttributes.clear();
  auto attrs = xmlNode.attributes();
  insp.xmlAttributes.reserve(attrs.size());
  for (const auto& attrName : attrs) {
    std::string formatted;
    if (!attrName.namespacePrefix.empty()) {
      formatted.reserve(attrName.namespacePrefix.size() + 1 + attrName.name.size());
      formatted.append(std::string_view(attrName.namespacePrefix));
      formatted.push_back(':');
      formatted.append(std::string_view(attrName.name));
    } else {
      formatted.assign(std::string_view(attrName.name));
    }

    InspectedAttributeEntry entry;
    entry.name = std::move(formatted);
    if (auto value = xmlNode.getAttribute(attrName); value.has_value()) {
      entry.value.assign(std::string_view(*value));
    }
    insp.xmlAttributes.push_back(std::move(entry));
  }

  // Computed-style snapshot. Stick to a short, stable list of the
  // properties reviewers care about most — a full dump would bloat
  // every frame and the operator<< format isn't user-friendly at
  // scale. The format here is "value (state)" extracted from
  // `operator<<` on the `Property`.
  insp.computedStyle.clear();
  const auto& cs = element.getComputedStyle();
  const auto appendProperty = [&insp](const char* name, const auto& prop) {
    std::ostringstream os;
    os << prop;
    // `operator<<` prepends "name:"; strip so the sidebar table can
    // show just the value column.
    std::string line = os.str();
    const std::string prefix = std::string(prop.name) + ":";
    if (line.rfind(prefix, 0) == 0) {
      line.erase(0, prefix.size());
      while (!line.empty() && line.front() == ' ') {
        line.erase(0, 1);
      }
    }
    insp.computedStyle.push_back({std::string(name), std::move(line)});
  };
  appendProperty("display", cs.display);
  appendProperty("visibility", cs.visibility);
  appendProperty("opacity", cs.opacity);
  appendProperty("fill", cs.fill);
  appendProperty("fill-opacity", cs.fillOpacity);
  appendProperty("stroke", cs.stroke);
  appendProperty("stroke-width", cs.strokeWidth);
  appendProperty("stroke-opacity", cs.strokeOpacity);
  appendProperty("color", cs.color);

  frame.hasInspectedElement = true;
}

void EditorBackendCore::appendPendingSourceWritebacks(FramePayload& frame) {
  std::vector<std::pair<TextPatch, WritebackReason>> patches;
  std::string source = editor_.cleanSourceText();

  if (auto dragWriteback = selectTool_.consumeCompletedDragWriteback(); dragWriteback.has_value()) {
    const auto appendDragPatch = [&](const SelectTool::CompletedDragWriteback& writeback) {
      auto patch = BuildTransformWritebackPatch(source, writeback.target, writeback.transform,
                                                /*sourceTransformAttributeValue=*/std::nullopt,
                                                /*restoreSourceTransformAttributeValue=*/false);
      if (patch.has_value()) {
        patches.push_back({std::move(*patch), WritebackReason::kAttributeEdit});
      }
    };
    appendDragPatch(*dragWriteback);
    for (const auto& extra : dragWriteback->extras) {
      appendDragPatch(extra);
    }
  }

  if (auto transformWriteback = editor_.consumeTransformWriteback();
      transformWriteback.has_value()) {
    auto patch = BuildTransformWritebackPatch(
        source, transformWriteback->target, transformWriteback->transform,
        transformWriteback->sourceTransformAttributeValue,
        transformWriteback->restoreSourceTransformAttributeValue);
    if (patch.has_value()) {
      patches.push_back({std::move(*patch), WritebackReason::kAttributeEdit});
    }
  }

  for (const auto& removeWriteback : editor_.consumeElementRemoveWritebacks()) {
    auto patch = buildElementRemoveWriteback(source, removeWriteback.target);
    if (patch.has_value()) {
      patches.push_back({std::move(*patch), WritebackReason::kElementRemoval});
    }
  }

  std::sort(patches.begin(), patches.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.first.offset > rhs.first.offset; });

  std::vector<TextPatch> textPatches;
  textPatches.reserve(patches.size());
  for (const auto& [patch, reason] : patches) {
    if (patch.offset > UINT32_MAX || patch.length > UINT32_MAX ||
        patch.offset + patch.length > UINT32_MAX) {
      continue;
    }
    frame.writebacks.push_back(MakeWritebackEntry(patch, reason));
    textPatches.push_back(patch);
  }

  if (!textPatches.empty()) {
    const ApplyPatchesResult result = applyPatches(source, textPatches);
    if (result.applied == textPatches.size() && result.rejectedBounds == 0u) {
      editor_.setCleanSourceText(source);
      AdjustLiveSourceRangesForPatches(editor_, textPatches);
    }
  }
}

FramePayload EditorBackendCore::buildFramePayload() {
  FramePayload frame;
  frame.frameId = frameIdCounter_++;

  if (editor_.hasDocument()) {
    editor_.flushFrame();

    auto& doc = editor_.document().document();
    // Avoid calling `setCanvasSize` on every frame: it unconditionally
    // calls `invalidateRenderTree()`, which in turn clamps the next
    // `renderFrame` out of the compositor's translation-only fast
    // path (`needsFullRebuild` gets set on the render-tree context).
    // Idempotence here is what makes steady-state drag cheap.
    //
    // Use a local shadow of the request — `doc.canvasSize()` reports
    // the aspect-fit-scaled result (e.g. setting (512, 384) on a
    // 400×400 viewBox reads back as (384, 384)), so comparing against
    // it would fire the setter every frame.
    const Vector2i requested(viewportWidth_, viewportHeight_);
    if (lastSetCanvasSize_ != requested) {
      doc.setCanvasSize(requested.x, requested.y);
      lastSetCanvasSize_ = requested;
    }

    // Lazily construct the compositor the first time we render a new
    // document. `compositor_` is cleared anywhere the document is
    // reparsed / replaced so the next render rebuilds against the new
    // registry.
    if (!compositor_.has_value()) {
      compositor_.emplace(doc, renderer_);
      compositorEntity_ = entt::null;
      compositorInteractionKind_ = donner::svg::compositor::InteractionHint::Selection;
      // The backend CPU-composites the compositor's cached bg/drag/fg
      // bitmaps into the wire-format frame (see `cpuComposeActive`
      // path below), so the compositor's own GPU main-compose is
      // redundant during an active drag. Skipping it cuts ~20-30 ms
      // off each drag frame on real-splash content by avoiding an
      // extra `beginFrame/drawImage×3/endFrame/takeSnapshot` on
      // Geode. Tests that opt out of CPU compose flip this off so the
      // main renderer actually produces the pixels `takeSnapshot`
      // returns.
      compositor_->setSkipMainComposeDuringSplit(cpuComposeEnabledForTesting_);
    }

    // Route selection / drag state into compositor promote/demote calls
    // so dragging takes the translation-only fast path instead of
    // re-rasterizing the whole document on every `kMove`. Priority:
    // an in-flight drag target wins over the pre-warmed selection.
    Entity desiredEntity = entt::null;
    donner::svg::compositor::InteractionHint desiredKind =
        donner::svg::compositor::InteractionHint::Selection;
    const auto activeDragPreview = selectTool_.activeDragPreview();
    if (activeDragPreview.has_value()) {
      desiredEntity = activeDragPreview->entity;
      desiredKind = donner::svg::compositor::InteractionHint::ActiveDrag;
    } else if (!editor_.selectedElements().empty()) {
      desiredEntity = editor_.selectedElements().front().entityHandle().entity();
      desiredKind = donner::svg::compositor::InteractionHint::Selection;
    }
    const bool compositorEntityChanged = desiredEntity != compositorEntity_;
    const bool compositorKindChanged =
        desiredEntity != entt::null && desiredKind != compositorInteractionKind_;
    if (compositorEntityChanged || compositorKindChanged) {
      if (compositorEntityChanged && compositorEntity_ != entt::null) {
        compositor_->demoteEntity(compositorEntity_);
      }
      if (compositorEntityChanged) {
        compositorEntity_ = entt::null;
        compositorInteractionKind_ = donner::svg::compositor::InteractionHint::Selection;
      }
      if (desiredEntity != entt::null && compositor_->promoteEntity(desiredEntity, desiredKind)) {
        compositorEntity_ = desiredEntity;
        compositorInteractionKind_ = desiredKind;
      }
    }

    // Compositor viewport must match `doc.canvasSize()` — the aspect-
    // fit-clamped size the main renderer uses via `setCanvasSize`
    // above. Using the raw `viewportWidth_ × viewportHeight_` instead
    // produces compositor-internal bitmaps at a different size than
    // the main renderer's `takeSnapshot()`, which then ships a
    // differently-sized `finalBitmap` once cpu-compose activates
    // (split-layer cache populated + at least one entity promoted).
    // The editor's display pane then sees the bitmap change size
    // the moment anything gets selected — reads as "the filter
    // group stops rendering" once a different element is picked,
    // because the UI scales the bigger bitmap down and crops. Issue
    // #582 follow-up repro: after selecting Big_lightning_glow then
    // Lightning_glow_dark, the snapshot jumped from 1310×752
    // (aspect-fit) to 1310×1726 (unclamped viewport) and the filter
    // group visually disappeared from the render pane.
    const Vector2i canvasSize = doc.canvasSize();
    donner::svg::RenderViewport viewport;
    viewport.size = Vector2d(canvasSize.x, canvasSize.y);
    viewport.devicePixelRatio = 1.0;

    const auto countersBefore = compositor_->fastPathCountersForTesting();
    compositor_->renderFrame(viewport);
    const auto countersAfter = compositor_->fastPathCountersForTesting();
    const bool fastPathOrCleanFrame =
        countersAfter.fastPathFrames > countersBefore.fastPathFrames ||
        countersAfter.noDirtyFrames > countersBefore.noDirtyFrames;

    const bool activeDrag = activeDragPreview.has_value();
    const bool splitPreviewAvailable =
        compositorEntity_ != entt::null && compositor_->hasSplitStaticLayers();
    if (splitPreviewAvailable) {
      const Transform2d composeOffset = compositor_->layerComposeOffset(compositorEntity_);
      const uint64_t layerGeneration = compositor_->layerGenerationOf(compositorEntity_);
      const Transform2d canvasFromDoc = doc.canvasFromDocumentTransform();
      const double docPerCanvasX = canvasFromDoc.data[0] != 0.0 ? 1.0 / canvasFromDoc.data[0] : 1.0;
      const double docPerCanvasY = canvasFromDoc.data[3] != 0.0 ? 1.0 / canvasFromDoc.data[3] : 1.0;
      Vector2d composeOffsetDoc = Vector2d::Zero();
      if (composeOffset.isTranslation()) {
        const Vector2d composeOffsetCanvas = composeOffset.translation();
        composeOffsetDoc =
            Vector2d(composeOffsetCanvas.x * docPerCanvasX, composeOffsetCanvas.y * docPerCanvasY);
      }

      const bool previewUploadNeeded = !compositedPreviewUploadsPrimed_ ||
                                       compositedPreviewUploadEntity_ != compositorEntity_ ||
                                       compositedPreviewUploadCanvasSize_ != canvasSize ||
                                       compositedPreviewUploadGeneration_ != layerGeneration ||
                                       (activeDrag && !fastPathOrCleanFrame);

      frame.hasCompositedPreview = true;
      frame.compositedPreviewActive = activeDrag;
      frame.compositedPreviewTranslationDoc[0] = composeOffsetDoc.x;
      frame.compositedPreviewTranslationDoc[1] = composeOffsetDoc.y;
      frame.hasCompositedPreviewBitmaps = previewUploadNeeded;

      if (previewUploadNeeded) {
        const auto renderPreviewOverlay = [&]() -> donner::svg::RendererBitmap {
          if (editor_.selectedElements().empty()) {
            return {};
          }
          if (!overlayRenderer_.has_value()) {
            overlayRenderer_.emplace();
          }
          donner::svg::RenderViewport overlayViewport;
          overlayViewport.size = Vector2d(canvasSize.x, canvasSize.y);
          overlayViewport.devicePixelRatio = 1.0;

          Transform2d overlayCanvasFromDoc = canvasFromDoc;
          if (composeOffset.isTranslation()) {
            const Vector2d composeOffsetCanvas = composeOffset.translation();
            overlayCanvasFromDoc = canvasFromDoc * Transform2d::Translate(-composeOffsetCanvas.x,
                                                                          -composeOffsetCanvas.y);
          }

          donner::svg::Renderer& overlayRenderer = *overlayRenderer_;
          overlayRenderer.beginFrame(overlayViewport);
          donner::editor::OverlayRenderer::drawChromeWithTransform(
              overlayRenderer, editor_.selectedElements(), /*marqueeRectDoc=*/std::nullopt,
              overlayCanvasFromDoc);
          overlayRenderer.endFrame();
          return overlayRenderer.takeSnapshot();
        };

        frame.compositedPreviewBackground = MakeFrameBitmapPayload(compositor_->backgroundBitmap());
        frame.compositedPreviewPromoted =
            MakeFrameBitmapPayload(compositor_->layerBitmapOf(compositorEntity_));
        frame.compositedPreviewForeground = MakeFrameBitmapPayload(compositor_->foregroundBitmap());
        frame.compositedPreviewOverlay = MakeFrameBitmapPayload(renderPreviewOverlay());
        compositedPreviewUploadsPrimed_ = true;
        compositedPreviewUploadEntity_ = compositorEntity_;
        compositedPreviewUploadCanvasSize_ = canvasSize;
        compositedPreviewUploadGeneration_ = layerGeneration;
      }
    } else {
      compositedPreviewUploadsPrimed_ = false;
      compositedPreviewUploadEntity_ = entt::null;
      compositedPreviewUploadCanvasSize_ = Vector2i(-1, -1);
      compositedPreviewUploadGeneration_ = 0;
    }

    // Fast-path: when the compositor has a single promoted layer with
    // cached bg/drag/fg bitmaps, skip the GPU main compose AND its
    // readback. Instead CPU-composite the compositor's internal
    // premul-alpha bitmaps (bg, drag layer at its canvas-space
    // compose offset, fg) into the wire-format bitmap. Saves a full
    // `beginFrame/drawImage×3/endFrame/takeSnapshot` cycle per drag
    // frame — on a 892×512 real-splash drag that's the difference
    // between 25–40 ms/frame (GPU compose+readback) and sub-10 ms
    // (three memcpy-like passes). `setSkipMainComposeDuringSplit`
    // below tells the compositor to skip its own GPU compose once
    // this CPU path is feasible.
    donner::svg::RendererBitmap snapshot;
    const bool cpuComposeActive = cpuComposeEnabledForTesting_ &&
                                  compositor_->hasSplitStaticLayers() &&
                                  compositorEntity_ != entt::null && !activeDrag;
    if (activeDrag && splitPreviewAvailable) {
      // The host already has bg/drag/fg textures. For steady active-drag
      // frames, ship only the updated translation in `compositedPreview`
      // and avoid building/uploading a full composed bitmap.
    } else if (cpuComposeActive) {
      const donner::svg::RendererBitmap& bg = compositor_->backgroundBitmap();
      const donner::svg::RendererBitmap& fg = compositor_->foregroundBitmap();
      const donner::svg::RendererBitmap& dragBitmap = compositor_->layerBitmapOf(compositorEntity_);
      const Transform2d composeOffset = compositor_->layerComposeOffset(compositorEntity_);
      int dragOffsetX = 0;
      int dragOffsetY = 0;
      if (composeOffset.isTranslation()) {
        const Vector2d t = composeOffset.translation();
        dragOffsetX = static_cast<int>(std::round(t.x));
        dragOffsetY = static_cast<int>(std::round(t.y));
      }
      // Output dimensions must match the compositor's internal
      // canvas — i.e. the `bg` / `dragBitmap` / `fg` bitmap size the
      // split-layer cache was built against. That size is
      // `doc.canvasSize()`, which applies the SVG's aspect-fit
      // (`setCanvasSize` clamps the requested `viewportWidth_ ×
      // viewportHeight_` to the viewBox's aspect ratio). If we sized
      // the output snapshot to the *unclamped* `viewportWidth_ ×
      // viewportHeight_` instead, `snapshot.pixels = bg.pixels` would
      // copy a too-small bg buffer into a too-big snapshot →
      // out-of-bounds composite writes, and the output dimensions
      // wouldn't match what the main-compose path (`renderer_
      // .takeSnapshot()`) produces for the same document, so the
      // editor's display path would see the bitmap suddenly change
      // size as soon as anything gets selected. Issue #582 post-fix
      // repro: a click on `<g filter>` made the scene jump from
      // 1310×752 (aspect-fit) to 1310×1726 (unclamped) in the
      // editor's render pane, which read as "the filter group
      // stopped rendering" because the UI was scaling the bigger
      // bitmap down + cropping.
      const Vector2i canvasSize = doc.canvasSize();
      snapshot.dimensions = canvasSize;
      snapshot.rowBytes = static_cast<size_t>(canvasSize.x) * 4u;
      snapshot.alphaType = donner::svg::AlphaType::Premultiplied;
      // Initialize the composite buffer by COPYING `bg` directly —
      // `bg` is already canvas-sized premul and covers every pixel,
      // so it's equivalent to (but strictly faster than) the earlier
      // "allocate a 7 MB zero-filled buffer, then `Over` bg onto it"
      // sequence. On a 1784×1024 HiDPI splash that tax was ~25 ms
      // alloc+zero + 6 ms bg-over-zero = ~31 ms of pure memory
      // traffic every drag frame, which is the difference between
      // hitting the 20 ms 60-fps budget and sitting near 45 ms.
      // Skipping the zero fill is safe because `bg` is already
      // opaque everywhere the scene draws and transparent-premul
      // everywhere else (matching the starting state we used to
      // zero-fill).
      snapshot.pixels = bg.pixels;
      CompositePremulOntoPremul(snapshot, dragBitmap, dragOffsetX, dragOffsetY);
      CompositePremulOntoPremul(snapshot, fg, 0, 0);
      UnpremultiplyInPlace(snapshot);
    } else {
      // Non-split path — the compositor wrote into the main renderer
      // the old way; read it back.
      snapshot = renderer_.takeSnapshot();
    }

    // Rasterize selection chrome into a dedicated transparent bitmap
    // via `overlayRenderer_`, then software-composite onto the
    // compositor's snapshot. Skip this while a drag preview is active:
    // the moving content already communicates the gesture, and on
    // Geode the extra overlay frame + readback is pure per-move tax.
    // The chrome comes back on the mouse-up settle frame.
    //
    // The chrome can't share `renderer_`'s frame because the compositor
    // has already called `endFrame` before returning; on Geode,
    // post-`endFrame` draws are no-ops against a finished command
    // buffer. A separate renderer opens its own frame lifecycle, so the
    // chrome draws are real.
    //
    // Render the overlay into a TIGHT bitmap sized around the selection's
    // canvas-space AABB (plus stroke + AA padding) instead of the full
    // canvas. On Geode a full-canvas overlay pays `beginFrame` (MSAA
    // texture alloc at 892×512×4) + render + `endFrame` + readback
    // every frame — ~20 ms on `donner_splash.svg`. A tight overlay on
    // a single letter is ~100×140 pixels, roughly 30× smaller, which
    // drops the overlay cost into the single-digit-ms range.
    const bool hasSelectionOrMarquee =
        !editor_.selectedElements().empty() || selectTool_.marqueeRect().has_value();
    const bool shouldDrawOverlay = hasSelectionOrMarquee && !activeDragPreview.has_value();
    if (shouldDrawOverlay && !snapshot.empty()) {
      if (!overlayRenderer_.has_value()) {
        overlayRenderer_.emplace();
      }
      donner::svg::Renderer& overlayRenderer = *overlayRenderer_;
      const Transform2d canvasFromDoc = doc.canvasFromDocumentTransform();
      const auto selectionBoundsDoc =
          donner::editor::SnapshotSelectionWorldBounds(editor_.selectedElements());

      // Union all element AABBs + the marquee rect (if any) into a
      // doc-space bounding rect, then map to canvas space.
      std::optional<Box2d> combinedDoc;
      const auto unionDoc = [&](const Box2d& b) {
        if (!combinedDoc) {
          combinedDoc = b;
        } else {
          combinedDoc->addBox(b);
        }
      };
      for (const Box2d& b : selectionBoundsDoc) unionDoc(b);
      if (selectTool_.marqueeRect().has_value()) unionDoc(*selectTool_.marqueeRect());

      Vector2i tightTopLeftPx(0, 0);
      Vector2i tightSizePx(viewportWidth_, viewportHeight_);
      if (combinedDoc.has_value()) {
        const Box2d canvasBounds = canvasFromDoc.transformBox(*combinedDoc);
        // Padding: selection strokes are 1–2 px, AA + marquee
        // strokes add a pixel or two. 8 px absolute + a relative 2 px
        // on each side keeps us well clear of being clipped.
        constexpr double kPadPx = 8.0;
        Vector2d tl(std::floor(std::max(0.0, canvasBounds.topLeft.x - kPadPx)),
                    std::floor(std::max(0.0, canvasBounds.topLeft.y - kPadPx)));
        Vector2d br(
            std::ceil(std::min<double>(viewportWidth_, canvasBounds.bottomRight.x + kPadPx)),
            std::ceil(std::min<double>(viewportHeight_, canvasBounds.bottomRight.y + kPadPx)));
        if (br.x > tl.x && br.y > tl.y) {
          tightTopLeftPx = Vector2i(static_cast<int>(tl.x), static_cast<int>(tl.y));
          tightSizePx = Vector2i(static_cast<int>(br.x - tl.x), static_cast<int>(br.y - tl.y));
        }
      }

      donner::svg::RenderViewport overlayViewport;
      overlayViewport.size = Vector2d(tightSizePx.x, tightSizePx.y);
      overlayViewport.devicePixelRatio = 1.0;

      // Shift the chrome into tight-local coords by pre-multiplying
      // `Translate(-tightTopLeft)` onto `canvasFromDoc`. After
      // `setTransform(T × canvasFromDoc)` inside `OverlayRenderer`,
      // the chrome draws land at `canvas - tightTopLeft`, which is
      // exactly the offset this bitmap represents when composited
      // back at `tightTopLeft`.
      const Transform2d tightCanvasFromDoc =
          canvasFromDoc * Transform2d::Translate(-tightTopLeftPx.x, -tightTopLeftPx.y);

      overlayRenderer.beginFrame(overlayViewport);
      donner::editor::OverlayRenderer::drawChromeWithTransform(
          overlayRenderer, editor_.selectedElements(), selectTool_.marqueeRect(),
          tightCanvasFromDoc);
      overlayRenderer.endFrame();
      donner::svg::RendererBitmap overlay = overlayRenderer.takeSnapshot();
      // Composite the tight overlay bitmap at its offset. Re-use
      // `CompositeOverlayOnto` but applied row-by-row with the offset
      // — inline the straight-alpha source-over here to avoid an
      // allocation-heavy full-canvas padding step.
      if (!overlay.empty()) {
        const int dstW = snapshot.dimensions.x;
        const int dstH = snapshot.dimensions.y;
        const int srcW = overlay.dimensions.x;
        const int srcH = overlay.dimensions.y;
        const int xStart = std::max(0, tightTopLeftPx.x);
        const int yStart = std::max(0, tightTopLeftPx.y);
        const int xEnd = std::min(dstW, tightTopLeftPx.x + srcW);
        const int yEnd = std::min(dstH, tightTopLeftPx.y + srcH);
        for (int y = yStart; y < yEnd; ++y) {
          uint8_t* dstRow = snapshot.pixels.data() + static_cast<size_t>(y) * snapshot.rowBytes;
          const uint8_t* srcRow =
              overlay.pixels.data() + static_cast<size_t>(y - tightTopLeftPx.y) * overlay.rowBytes;
          for (int x = xStart; x < xEnd; ++x) {
            const size_t dstOff = static_cast<size_t>(x) * 4;
            const size_t srcOff = static_cast<size_t>(x - tightTopLeftPx.x) * 4;
            const uint32_t sa = srcRow[srcOff + 3];
            if (sa == 0u) continue;
            if (sa == 255u) {
              dstRow[dstOff + 0] = srcRow[srcOff + 0];
              dstRow[dstOff + 1] = srcRow[srcOff + 1];
              dstRow[dstOff + 2] = srcRow[srcOff + 2];
              dstRow[dstOff + 3] = 255u;
              continue;
            }
            const uint32_t da = dstRow[dstOff + 3];
            const uint32_t invA = 255u - sa;
            const uint32_t daInv = (da * invA + 127u) / 255u;
            const uint32_t outA = sa + daInv;
            const auto blend = [sa, daInv, outA](uint32_t s, uint32_t d) -> uint8_t {
              const uint32_t num = s * sa + d * daInv;
              return static_cast<uint8_t>((num + (outA >> 1u)) / outA);
            };
            dstRow[dstOff + 0] = blend(srcRow[srcOff + 0], dstRow[dstOff + 0]);
            dstRow[dstOff + 1] = blend(srcRow[srcOff + 1], dstRow[dstOff + 1]);
            dstRow[dstOff + 2] = blend(srcRow[srcOff + 2], dstRow[dstOff + 2]);
            dstRow[dstOff + 3] = static_cast<uint8_t>(outA);
          }
        }
      }
    }
    if (!snapshot.empty()) {
      frame.hasFinalBitmap = true;
      frame.finalBitmapWidth = snapshot.dimensions.x;
      frame.finalBitmapHeight = snapshot.dimensions.y;
      frame.finalBitmapRowBytes = static_cast<uint32_t>(snapshot.rowBytes);
      frame.finalBitmapAlphaType = static_cast<uint8_t>(snapshot.alphaType);
      frame.finalBitmapPixels = std::move(snapshot.pixels);
    }
    frame.statusKind = FrameStatusKind::kRendered;

    // Populate selection entries. Emit one per selected element, ordered
    // by `editor_.selectedElements()`. Use `SnapshotSelectionWorldBounds`
    // per-element so non-geometry `<g>` selections (filter groups, plain
    // groups) get an AABB unioned across the subtree's renderable
    // geometry. Previously this skipped any non-`SVGGeometryElement`,
    // which meant `selections` came back EMPTY whenever the user elevated
    // a click to a `<g filter>` — blinding test harnesses that want to
    // sanity-check "what actually got selected".
    //
    // Per-element query preserves 1:1 index correspondence with
    // `selectedElements()` even when some selected elements have no
    // renderable geometry (e.g. `<defs>`, empty `<g>`); those get an
    // all-zero bbox.
    for (const auto& elem : editor_.selectedElements()) {
      FrameSelectionEntry entry;
      entry.hasTransform = false;
      entry.handleMask = 0xFF;
      std::array<svg::SVGElement, 1> single{elem};
      const auto bounds = donner::editor::SnapshotSelectionWorldBounds(single);
      if (!bounds.empty()) {
        const Box2d& bb = bounds.front();
        entry.bbox[0] = bb.topLeft.x;
        entry.bbox[1] = bb.topLeft.y;
        entry.bbox[2] = bb.bottomRight.x;
        entry.bbox[3] = bb.bottomRight.y;
      }
      frame.selections.push_back(entry);
    }

    // Tree summary.
    populateTreeSummary(frame.tree);

    // Inspector snapshot for the single selected element, if any.
    populateInspectedElement(frame);

    appendPendingSourceWritebacks(frame);

    // Document viewBox — the coordinate space every bbox / marquee /
    // pointer event on this frame is in. The host needs it to map
    // screen pixels ↔ document-space points. Prefer an explicit
    // `viewBox="..."` attribute; fall back to `(0, 0, width, height)`
    // from the root's intrinsic length attributes; fall back further
    // to SVG's default `(300, 150)` when neither is set. Unit coercion
    // is best-effort: we use the raw attribute value so a `<svg
    // width="892">` lands as 892 user units regardless of the unit
    // tag, which matches what the backend's own hit-test uses.
    auto rootEl = doc.svgElement();
    Box2d viewBox;
    if (auto explicitVb = rootEl.viewBox()) {
      viewBox = *explicitVb;
    } else {
      const double w = rootEl.width().has_value() ? rootEl.width()->value : 300.0;
      const double h = rootEl.height().has_value() ? rootEl.height()->value : 150.0;
      viewBox = Box2d::FromXYWH(0.0, 0.0, w, h);
    }
    frame.hasDocumentViewBox = true;
    frame.documentViewBox[0] = viewBox.topLeft.x;
    frame.documentViewBox[1] = viewBox.topLeft.y;
    frame.documentViewBox[2] = viewBox.width();
    frame.documentViewBox[3] = viewBox.height();

    // Marquee (rubber-band selection rect). `SelectTool` owns the
    // state during a drag-on-empty-space; we ship the doc-space
    // rect into `FramePayload.marquee` so the host (or the overlay
    // renderer baked into the bitmap) can draw the chrome that goes
    // on top of the content. The overlay renderer also reads this
    // above, so the same rect shows up twice: once painted into the
    // final bitmap, once as metadata the host can use for hit-test
    // proximity hints. Both reads pull from `SelectTool` directly —
    // single source of truth.
    if (auto marquee = selectTool_.marqueeRect()) {
      frame.hasMarquee = true;
      frame.marquee[0] = marquee->topLeft.x;
      frame.marquee[1] = marquee->topLeft.y;
      frame.marquee[2] = marquee->bottomRight.x;
      frame.marquee[3] = marquee->bottomRight.y;
    }
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
  if (editor_.hasDocument() && rep.bytes == editor_.cleanSourceText()) {
    return buildFramePayload();
  }

  const auto selectionTargets = CaptureSelectionTargets(editor_);
  bumpEntityGeneration();
  const bool loaded = editor_.loadFromString(rep.bytes);
  if (loaded) {
    RestoreSelectionTargets(editor_, selectionTargets);
  }
  editor_.setCleanSourceText(rep.bytes);
  return buildFramePayload();
}

FramePayload EditorBackendCore::handleApplySourcePatch(const ApplySourcePatchPayload& patch) {
  std::string source = editor_.cleanSourceText();
  if (patch.start <= patch.end && patch.end <= source.size()) {
    source.replace(patch.start, patch.end - patch.start, patch.newText);
    if (editor_.hasDocument() && source == editor_.cleanSourceText()) {
      return buildFramePayload();
    }
    const auto selectionTargets = CaptureSelectionTargets(editor_);
    bumpEntityGeneration();
    const bool loaded = editor_.loadFromString(source);
    if (loaded) {
      RestoreSelectionTargets(editor_, selectionTargets);
    }
    editor_.setCleanSourceText(source);
  }
  return buildFramePayload();
}

FramePayload EditorBackendCore::handlePointerEvent(const PointerEventPayload& ptr) {
  if (editor_.hasDocument()) {
    const Vector2d docPoint(ptr.documentX, ptr.documentY);
    // `SelectTool` expects the shift modifier through its own
    // `MouseModifiers` struct; map the protocol bitmask across.
    MouseModifiers modifiers;
    modifiers.shift = (ptr.modifiers & 0x1u) != 0;
    switch (ptr.phase) {
      case PointerPhase::kDown: selectTool_.onMouseDown(editor_, docPoint, modifiers); break;
      case PointerPhase::kMove:
        // `buttonHeld` is only meaningful while a drag is in flight.
        // `ptr.buttons` bit 0 = primary; default-treat any held
        // button as drag continuation since the host only forwards
        // moves during a drag anyway.
        selectTool_.onMouseMove(editor_, docPoint, /*buttonHeld=*/ptr.buttons != 0, modifiers);
        break;
      case PointerPhase::kUp:
      case PointerPhase::kCancel:
        // Treat cancel like an up so any in-flight drag commits or
        // cleanly aborts rather than leaving `dragState_` populated.
        selectTool_.onMouseUp(editor_, docPoint);
        break;
      case PointerPhase::kEnter:
      case PointerPhase::kLeave:
        // Focus-tracking events; no tool state change required. Host
        // uses these to drive hover/cursor UI, not document state.
        break;
    }
  }
  return buildFramePayload();
}

FramePayload EditorBackendCore::handleKeyEvent(const KeyEventPayload& key) {
  if (IsDeleteKey(key) && editor_.hasDocument() && editor_.hasSelection()) {
    const std::vector<svg::SVGElement> selection = editor_.selectedElements();
    bool deletedAny = false;
    for (const auto& element : selection) {
      if (!element.parentElement().has_value()) {
        continue;
      }
      if (auto target = captureAttributeWritebackTarget(element); target.has_value()) {
        editor_.enqueueElementRemoveWriteback(
            EditorApp::CompletedElementRemoveWriteback{.target = std::move(*target)});
      }
      editor_.applyMutation(EditorCommand::DeleteElementCommand(element));
      deletedAny = true;
    }
    editor_.clearSelection();
    if (deletedAny) {
      resetCompositorState();
    }
  }
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
      default: break;
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
  // SVG text export: return the last-persisted source bytes. This is
  // the same text the host's `kReplaceSource` path feeds us, so drag
  // writebacks / attribute edits / source-pane keystrokes have
  // already been incorporated by the time the export request lands.
  // PNG/other formats are not yet wired on the backend (would need to
  // serialize `finalBitmapPixels` into a PNG encoder); the host
  // currently only invokes `kExport` for the SVG save-as flow.
  if (req.format == ExportFormat::kSvgText && editor_.hasDocument()) {
    const std::string& svg = editor_.cleanSourceText();
    resp.bytes.assign(svg.begin(), svg.end());
  }
  return resp;
}

FramePayload EditorBackendCore::handleAttachSharedTexture(
    const AttachSharedTexturePayload& attach) {
  // Translate the wire payload into a `BridgeTextureHandle` and ask
  // the platform-specific factory to import it. Unknown `kind`
  // values fall through to the stub so the backend stays functional
  // — the host will just see `finalBitmapPixels` instead of
  // zero-copy rendering.
  bridge::BridgeTextureHandle handle;
  handle.kind = static_cast<bridge::BridgeHandleKind>(attach.kind);
  handle.handle = attach.handle;
  handle.dimensions = Vector2i(attach.width, attach.height);
  handle.rowBytes = attach.rowBytes;

  switch (handle.kind) {
#if defined(__APPLE__)
    case bridge::BridgeHandleKind::kIOSurfaceMacOS:
      bridge_ = bridge::MakeBackend_macOS(handle);
      break;
#endif
    case bridge::BridgeHandleKind::kCpuStub:
    default:
      // Unknown / unsupported kind → stub. The host gets an
      // acknowledgment frame but subsequent renders keep shipping
      // `finalBitmapPixels`.
      bridge_ = bridge::MakeBackendStub(handle);
      break;
  }
  return buildFramePayload();
}

}  // namespace donner::editor::sandbox
