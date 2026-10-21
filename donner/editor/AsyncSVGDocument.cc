#include "donner/editor/AsyncSVGDocument.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/editor/EditorParseOptions.h"
#include "donner/editor/LockState.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/SVGStyleElement.h"
#include "donner/svg/SVGTextContentElement.h"
#include "donner/svg/compositor/CompositorController.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::editor {

namespace {

constexpr std::size_t kMaxPublishedParseDiagnostics = 256;

std::vector<ParseDiagnostic> CopyWarnings(const ParseWarningSink& sink) {
  const auto& warnings = sink.warnings();
  const std::size_t count = std::min(warnings.size(), kMaxPublishedParseDiagnostics);
  return std::vector<ParseDiagnostic>(warnings.begin(), warnings.begin() + count);
}

bool ExceedsReplacementBatchNodeLimit(svg::SVGDocument& document, const EditorCommand& command) {
  const std::uint64_t remaining = document.xmlDocument().remainingSourceEditTreeNodeBudget();
  std::uint64_t needed = 0;
  for (const EditorCommand::ElementReplacement& replacement : command.replacements) {
    std::vector<svg::SVGElement> pending{replacement.replacement};
    while (!pending.empty()) {
      svg::SVGElement element = pending.back();
      pending.pop_back();
      if (!xml::XMLNode::TryCast(element.entityHandle()).has_value()) {
        if (needed == remaining) {
          return true;
        }
        ++needed;
      }
      for (std::optional<svg::SVGElement> child = element.firstChild(); child.has_value();
           child = child->nextSibling()) {
        pending.push_back(*child);
      }
    }
  }
  return false;
}

// Removing each original must not be refused once its replacement is staged, so check every
// condition removal depends on before staging anything.
bool OriginalsAreRemovable(const EditorCommand& command) {
  std::vector<Entity> originals;
  originals.reserve(command.replacements.size());
  for (const EditorCommand::ElementReplacement& replacement : command.replacements) {
    const std::optional<xml::XMLNode> node =
        xml::XMLNode::TryCast(replacement.original.entityHandle());
    if (!node.has_value() || !node->getNodeLocation().has_value() ||
        replacement.original.parentElement() != std::optional(replacement.parent)) {
      return false;
    }
    const Entity entity = replacement.original.entityHandle().entity();
    if (std::find(originals.begin(), originals.end(), entity) != originals.end()) {
      return false;
    }
    originals.push_back(entity);
  }
  // A nested original's source range moves with its ancestor's removal.
  for (const EditorCommand::ElementReplacement& replacement : command.replacements) {
    for (std::optional<svg::SVGElement> ancestor = replacement.parent; ancestor.has_value();
         ancestor = ancestor->parentElement()) {
      if (std::find(originals.begin(), originals.end(), ancestor->entityHandle().entity()) !=
          originals.end()) {
        return false;
      }
    }
  }
  return true;
}

struct ReplacementBatchEdits {
  bool failed = false;
  bool sourceRestored = false;
  std::optional<ParseDiagnostic> failure;
  std::vector<xml::XMLSourceDelta> sourceDeltas;
};

/// Staged replacement subtrees with the single source insertion each one made, oldest first.
struct StagedReplacements {
  std::vector<svg::SVGElement> elements;
  std::vector<xml::XMLSourceDelta> insertions;
  /// False when an insertion's source edit cannot be reverted exactly.
  bool revertible = true;
};

void AppendReplacementDeltas(ReplacementBatchEdits& edits,
                             const xml::ApplySourceEditResult& result) {
  edits.sourceDeltas.insert(edits.sourceDeltas.end(), result.sourceDeltas.begin(),
                            result.sourceDeltas.end());
}

void StageReplacementSubtrees(svg::SVGDocument& document, const EditorCommand& command,
                              ReplacementBatchEdits& edits, StagedReplacements& staged) {
  for (const EditorCommand::ElementReplacement& replacement : command.replacements) {
    xml::ApplySourceEditResult inserted =
        document.insertElement(replacement.parent, replacement.replacement, replacement.original);
    AppendReplacementDeltas(edits, inserted);
    if (inserted.applied && inserted.sourceDeltas.size() == 1) {
      staged.elements.push_back(replacement.replacement);
      staged.insertions.push_back(inserted.sourceDeltas.front());
    } else if (!inserted.sourceDeltas.empty()) {
      staged.revertible = false;
    }
    if (!inserted.applied || inserted.diagnostic.has_value()) {
      edits.failure = std::move(inserted.diagnostic);
      edits.failed = true;
      return;
    }
  }
}

void RemoveReplacementOriginals(svg::SVGDocument& document, const EditorCommand& command,
                                ReplacementBatchEdits& edits) {
  for (const EditorCommand::ElementReplacement& replacement : command.replacements) {
    xml::ApplySourceEditResult removed = document.removeElement(replacement.original);
    AppendReplacementDeltas(edits, removed);
    if (!removed.applied || removed.diagnostic.has_value()) {
      edits.failure = std::move(removed.diagnostic);
      edits.failed = true;
      return;
    }
  }
}

// Reverting the exact insertion deltas also removes the indentation each insertion added, so a
// refusal leaves byte-identical source and needs no document replacement.
void RollbackStagedReplacements(svg::SVGDocument& document, const StagedReplacements& staged,
                                ReplacementBatchEdits& edits) {
  if (staged.elements.empty() || !staged.revertible) {
    return;
  }
  const xml::ApplySourceEditResult reverted =
      document.revertElementInsertions(staged.elements, staged.insertions);
  AppendReplacementDeltas(edits, reverted);
}

ReplacementBatchEdits ApplyReplacementBatchEdits(svg::SVGDocument& document,
                                                 const EditorCommand& command,
                                                 std::string_view sourceBefore) {
  ReplacementBatchEdits edits;
  StagedReplacements staged;
  staged.elements.reserve(command.replacements.size());
  staged.insertions.reserve(command.replacements.size());
  // Nested DOM setters reuse this scope; no reader can observe a partial batch.
  [[maybe_unused]] svg::DocumentWriteAccess access = document.writeAccess();
  if (document.source() != sourceBefore) {
    edits.failed = true;
    edits.sourceRestored = true;
    return edits;
  }
  if (!OriginalsAreRemovable(command)) {
    edits.failure = ParseDiagnostic::Warning(
        "Text-to-outline selection no longer matches the document", FileOffset::Offset(0));
    edits.failed = true;
  } else if (ExceedsReplacementBatchNodeLimit(document, command)) {
    edits.failure = ParseDiagnostic::Warning(
        "Text-to-outline selection exceeds the tree-node limit", FileOffset::Offset(0));
    edits.failed = true;
  }
  if (!edits.failed) {
    StageReplacementSubtrees(document, command, edits, staged);
  }
  if (!edits.failed) {
    RemoveReplacementOriginals(document, command, edits);
  }
  if (edits.failed) {
    RollbackStagedReplacements(document, staged, edits);
    edits.sourceRestored = document.source() == sourceBefore &&
                           std::all_of(command.replacements.begin(), command.replacements.end(),
                                       [](const EditorCommand::ElementReplacement& replacement) {
                                         return replacement.original.parentElement().has_value();
                                       });
  }
  return edits;
}

bool DeferredCommandIsLocked(const EditorCommand& command) {
  if (!command.deferLayerLockCheck) {
    return false;
  }
  if (command.kind == EditorCommand::Kind::ReplaceElements) {
    return std::any_of(command.replacements.begin(), command.replacements.end(),
                       [](const EditorCommand::ElementReplacement& replacement) {
                         return IsLocked(replacement.original);
                       });
  }
  return command.element.has_value() && IsLocked(*command.element);
}

bool InvalidatesExistingCompositedPixels(const EditorCommand& command) {
  switch (command.kind) {
    case EditorCommand::Kind::SetAttribute:
    case EditorCommand::Kind::RemoveAttribute:
    case EditorCommand::Kind::SetTextContent: return command.element.has_value();

    case EditorCommand::Kind::SetTransform:
    case EditorCommand::Kind::ReplaceDocument:
    case EditorCommand::Kind::InsertElement:
    case EditorCommand::Kind::ReplaceElements:
    case EditorCommand::Kind::DeleteElement:
    case EditorCommand::Kind::CutShapes:
    case EditorCommand::Kind::PasteShapes:
    case EditorCommand::Kind::InsertText: return false;
  }
  return false;
}

}  // namespace

AsyncSVGDocument::AsyncSVGDocument() = default;

void AsyncSVGDocument::publishParseDiagnostics(std::vector<ParseDiagnostic> warnings,
                                               std::optional<ParseDiagnostic> fatalError) {
  lastParseError_ = fatalError;
  if (warnings.size() > kMaxPublishedParseDiagnostics) {
    warnings.resize(kMaxPublishedParseDiagnostics);
  }
  if (fatalError.has_value() && warnings.size() == kMaxPublishedParseDiagnostics) {
    warnings.pop_back();
  }
  parseDiagnostics_ = std::move(warnings);
  if (fatalError.has_value()) {
    parseDiagnostics_.push_back(std::move(*fatalError));
  }
  ++parseDiagnosticsRevision_;
}

void AsyncSVGDocument::captureSourceRecoveryPolicy() {
  document_->withReadAccess([&](svg::DocumentReadAccess&) {
    const auto store = document_->xmlDocument().sharedSourceStore();
    const auto resources = document_->resourcePolicySnapshot();
    if (!store || !resources) {
      sourceRecoveryPolicy_.reset();
      return;
    }
    sourceRecoveryPolicy_ = SourceRecoveryPolicy{
        .parserOptions = *xml::XMLParser::SourceOptions(document_->xmlDocument()),
        .sourceLimits = store->resourceLimits(),
        .sourceStore = store,
        .resources = *resources,
    };
  });
}

bool AsyncSVGDocument::SourceRecoveryPolicy::matchesSource(const xml::XMLDocument& document) const {
  const auto* store = document.sourceStore();
  if (!store || sourceStore.lock().get() != store) {
    return false;
  }
  const auto& limits = store->resourceLimits();
  return xml::XMLParser::SourceOptions(document) == parserOptions &&
         limits.maximumSourceSize == sourceLimits.maximumSourceSize &&
         limits.maximumLiveAnchorCount == sourceLimits.maximumLiveAnchorCount &&
         limits.maximumAnchorUpdateWorkPerEdit == sourceLimits.maximumAnchorUpdateWorkPerEdit;
}

bool AsyncSVGDocument::usesEditorParserForSourceRecovery() const {
  return sourceRecoveryState().has_value();
}

std::optional<EditorCommand::SourceRecoveryState> AsyncSVGDocument::sourceRecoveryState() const {
  if (!document_ || !sourceRecoveryPolicy_) {
    return std::nullopt;
  }
  return document_->withReadAccess(
      [&](svg::DocumentReadAccess&) -> std::optional<EditorCommand::SourceRecoveryState> {
        if (!sourceRecoveryPolicy_->matchesSource(document_->xmlDocument()) ||
            document_->resourcePolicySnapshot() != sourceRecoveryPolicy_->resources) {
          return std::nullopt;
        }
        return EditorCommand::SourceRecoveryState{documentGeneration(), document_->sourceVersion()};
      });
}

void AsyncSVGDocument::setDocument(svg::SVGDocument document) {
  document_ = std::move(document);
  sourceRecoveryPolicy_.reset();
  queue_.clear();
  unmirroredSourceDeltas_.clear();
  unmirroredSourceDeltasOverflowed_ = false;
  publishParseDiagnostics({}, std::nullopt);
  pendingStructuralRemap_.clear();
  frameVersion_.fetch_add(1, std::memory_order_release);
  documentGeneration_.fetch_add(1, std::memory_order_release);
  ++nonTransformRevision_;
}

AsyncSVGDocument::ReplaceKind AsyncSVGDocument::setDocumentMaybeStructural(
    svg::SVGDocument newDocument) {
  sourceRecoveryPolicy_.reset();
  // If there's no current document, a structural remap isn't meaningful -
  // just install the new one like a regular `setDocument`.
  if (!document_.has_value()) {
    setDocument(std::move(newDocument));
    return ReplaceKind::FullReplace;
  }
  const std::optional<Vector2i> preservedCanvasSize = document_->canvasSizeOverride();

  // Build the remap BEFORE the swap. The walker needs both documents'
  // XML trees live; once we move `newDocument` into `document_`, the
  // old document is gone. Note this runs on the UI thread with no
  // locking - consistent with the existing `setDocument` contract that
  // only the UI thread touches the document outside a render.
  auto remap = svg::compositor::BuildStructuralEntityRemap(*document_, newDocument);
  const bool structural = !remap.empty();
  if (structural && preservedCanvasSize.has_value()) {
    // Preserve only an explicit canvas constraint, including its original aspect-fit bounds.
    // Copying the derived canvasSize() would freeze automatic SVG sizing across source edits.
    newDocument.setCanvasSize(preservedCanvasSize->x, preservedCanvasSize->y);
  }

  // Swap the document. We can't call `setDocument` directly because it
  // clears `pendingStructuralRemap_` - we want to populate it right
  // after.
  document_ = std::move(newDocument);
  queue_.clear();
  unmirroredSourceDeltas_.clear();
  unmirroredSourceDeltasOverflowed_ = false;
  publishParseDiagnostics({}, std::nullopt);
  frameVersion_.fetch_add(1, std::memory_order_release);
  documentGeneration_.fetch_add(1, std::memory_order_release);
  ++nonTransformRevision_;

  if (structural) {
    pendingStructuralRemap_ = std::move(remap);
    return ReplaceKind::Structural;
  }
  pendingStructuralRemap_.clear();
  return ReplaceKind::FullReplace;
}

void AsyncSVGDocument::appendUnmirroredSourceDeltas(
    const std::vector<xml::XMLSourceDelta>& sourceDeltas) {
  if (unmirroredSourceDeltasOverflowed_) {
    return;
  }

  if (unmirroredSourceDeltas_.size() + sourceDeltas.size() > kMaxUnmirroredSourceDeltas) {
    unmirroredSourceDeltas_.clear();
    unmirroredSourceDeltas_.shrink_to_fit();
    unmirroredSourceDeltasOverflowed_ = true;
    return;
  }

  unmirroredSourceDeltas_.insert(unmirroredSourceDeltas_.end(), sourceDeltas.begin(),
                                 sourceDeltas.end());
}

bool AsyncSVGDocument::flushFrame() {
  lastFlushResult_ = {};
  if (queue_.empty()) {
    return false;
  }

  queue_.discardStaleSourceRecoveries(sourceRecoveryState());
  auto queueFlush = queue_.flush();
  if (queueFlush.effectiveCommands.empty()) {
    return false;
  }

  lastFlushResult_ = FlushResult{
      .appliedCommands = false,
      .onlyTransformCommands = true,
      .replacedDocument = queueFlush.hadReplaceDocument,
      .preserveUndoOnReparse = queueFlush.preserveUndoOnReparse,
  };
  std::unordered_map<Entity, Entity> activeStructuralRemap;
  for (EditorCommand& cmd : queueFlush.effectiveCommands) {
    if (applyQueuedCommand(cmd, activeStructuralRemap)) {
      break;
    }
  }

  if (!lastFlushResult_.appliedCommands) {
    lastFlushResult_ = {};
    return false;
  }
  if (!lastFlushResult_.onlyTransformCommands) {
    ++nonTransformRevision_;
  }
  appendUnmirroredSourceDeltas(lastFlushResult_.sourceDeltas);
  frameVersion_.fetch_add(1, std::memory_order_release);
  return true;
}

bool AsyncSVGDocument::applyQueuedCommand(
    EditorCommand& command, std::unordered_map<Entity, Entity>& activeStructuralRemap) {
  if (!activeStructuralRemap.empty()) {
    remapCommandTargets(&command, activeStructuralRemap);
  }
  if (DeferredCommandIsLocked(command)) {
    return false;
  }
  // A replacement batch can be refused without changing the document.
  if (command.kind != EditorCommand::Kind::ReplaceElements) {
    recordAppliedCommand(command);
  }
  applyOne(command);
  if (lastFlushResult_.rolledBackReplacementBatch) {
    // The rollback replaced the document. Later handles belong to the old entity space.
    return true;
  }
  if (command.kind == EditorCommand::Kind::ReplaceDocument ||
      command.kind == EditorCommand::Kind::CutShapes ||
      command.kind == EditorCommand::Kind::PasteShapes) {
    activeStructuralRemap = pendingStructuralRemap_;
  }
  return false;
}

void AsyncSVGDocument::recordAppliedCommand(const EditorCommand& cmd) {
  lastFlushResult_.appliedCommands = true;
  lastFlushResult_.onlyTransformCommands &= cmd.kind == EditorCommand::Kind::SetTransform;
  if (cmd.kind == EditorCommand::Kind::DeleteElement) {
    lastFlushResult_.removedElements = true;
  }
  if (InvalidatesExistingCompositedPixels(cmd)) {
    lastFlushResult_.cacheInvalidatedElements.push_back(cmd.element->unsafeEntityHandle().entity());
  }
}

bool AsyncSVGDocument::dispatchMouseEvent(svg::EventType type, const svg::SVGElement& target) {
  if (!document_) {
    return false;
  }
  const auto access = document_->tryWriteAccess();
  if (!access.has_value() || !document_->dispatchMouseEvent(type, target)) {
    return false;
  }
  frameVersion_.fetch_add(1, std::memory_order_release);
  ++nonTransformRevision_;
  return true;
}

bool AsyncSVGDocument::restartAnimations() {
  if (!document_) {
    return false;
  }
  const auto access = document_->tryWriteAccess();
  if (!access.has_value()) {
    return false;
  }
  document_->restartAnimations();
  frameVersion_.fetch_add(1, std::memory_order_release);
  ++nonTransformRevision_;
  return true;
}

bool AsyncSVGDocument::setAnimationTime(double seconds) {
  if (!document_ || !std::isfinite(seconds)) {
    return false;
  }
  const auto access = document_->tryWriteAccess();
  if (!access.has_value() || document_->currentTime() == seconds) {
    return false;
  }
  document_->setTime(seconds);
  frameVersion_.fetch_add(1, std::memory_order_release);
  ++nonTransformRevision_;
  return true;
}

bool AsyncSVGDocument::refreshFontResources() {
  if (!document_ || !document_->refreshFontResources()) {
    return false;
  }
  ++fontResourceRevision_;
  frameVersion_.fetch_add(1, std::memory_order_release);
  ++nonTransformRevision_;
  return true;
}

void AsyncSVGDocument::remapCommandTargets(EditorCommand* command,
                                           const std::unordered_map<Entity, Entity>& remap) {
  if (!document_.has_value() || command == nullptr) {
    return;
  }

  Registry& registry = document_->unsafeRegistry();
  auto remapElement = [&](std::optional<svg::SVGElement>* element) {
    if (element == nullptr || !element->has_value()) {
      return;
    }

    const Entity oldEntity = (*element)->unsafeEntityHandle().entity();
    auto it = remap.find(oldEntity);
    if (it == remap.end() || it->second == entt::null || !registry.valid(it->second)) {
      return;
    }

    *element = svg::SVGElement(EntityHandle(registry, it->second));
  };

  remapElement(&command->element);
  remapElement(&command->parentElement);
  remapElement(&command->referenceElement);
  for (EditorCommand::ElementReplacement& replacement : command->replacements) {
    std::optional<svg::SVGElement> parent = replacement.parent;
    std::optional<svg::SVGElement> original = replacement.original;
    std::optional<svg::SVGElement> next = replacement.replacement;
    remapElement(&parent);
    remapElement(&original);
    remapElement(&next);
    replacement = EditorCommand::ElementReplacement{*parent, *original, *next};
  }
}

xml::ApplySourceEditResult AsyncSVGDocument::applySourceEdit(const xml::XMLEditIntent& intent) {
  if (!document_.has_value()) {
    xml::ApplySourceEditResult result;
    result.diagnostic =
        ParseDiagnostic::Error("Cannot apply source edit without a loaded document", intent.range);
    publishParseDiagnostics({}, result.diagnostic);
    return result;
  }

  xml::ApplySourceEditResult result = document_->applySourceEdit(intent);
  publishParseDiagnostics({}, result.diagnostic);

  if (result.applied || !result.mutations.empty()) {
    frameVersion_.fetch_add(1, std::memory_order_release);
    ++nonTransformRevision_;
  }

  return result;
}

bool AsyncSVGDocument::loadFromString(std::string_view svgBytes) {
  ParseWarningSink sink;
  auto result = svg::parser::SVGParser::ParseSVG(svgBytes, sink, EditorParseOptions());
  if (result.hasError()) {
    // Stash the diagnostic so the source pane can show a line marker.
    // Leave the existing document in place - the user can keep editing
    // until the source parses again. We deliberately do NOT bump the
    // frame version: the renderer's view of the document is unchanged,
    // and the source pane polls `lastParseError()` directly.
    publishParseDiagnostics(CopyWarnings(sink), std::move(result.error()));
    return false;
  }
  setDocument(std::move(result.result()));
  captureSourceRecoveryPolicy();
  publishParseDiagnostics(CopyWarnings(sink), std::nullopt);
  return true;
}

void AsyncSVGDocument::applyReplacementBatch(const EditorCommand& command) {
  if (command.replacements.empty()) {
    return;
  }

  const std::string sourceBefore = document_->withReadAccess(
      [this](svg::DocumentReadAccess&) { return std::string(document_->source()); });
  const bool priorApplied = lastFlushResult_.appliedCommands;
  ReplacementBatchEdits edits = ApplyReplacementBatchEdits(*document_, command, sourceBefore);
  if (!edits.failed) {
    lastFlushResult_.sourceDeltas.insert(lastFlushResult_.sourceDeltas.end(),
                                         edits.sourceDeltas.begin(), edits.sourceDeltas.end());
    recordAppliedCommand(command);
    lastFlushResult_.removedElements = true;
    return;
  }

  if (!edits.sourceRestored) {
    // Fail-safe for a broken edit invariant: removal of a checked original or the exact revert of
    // its own insertions was refused. Earlier edits in this flush are incorporated in sourceBefore.
    ParseWarningSink rollbackSink;
    auto rollbackParsed =
        svg::parser::SVGParser::ParseSVG(sourceBefore, rollbackSink, EditorParseOptions());
    if (rollbackParsed.hasError()) {
      publishParseDiagnostics(CopyWarnings(rollbackSink), std::move(rollbackParsed.error()));
      return;
    }
    (void)setDocumentMaybeStructural(std::move(rollbackParsed).result());
    captureSourceRecoveryPolicy();
    lastFlushResult_.sourceDeltas.clear();
    recordAppliedCommand(command);
    lastFlushResult_.replacedDocument = true;
    lastFlushResult_.preserveUndoOnReparse = true;
    lastFlushResult_.rolledBackReplacementBatch = true;
    lastFlushResult_.hadPriorAppliedCommandsOnRollback = priorApplied;
  }
  const ParseDiagnostic refusal = edits.failure.value_or(
      ParseDiagnostic::Error("Text-to-outline batch was refused", FileOffset::Offset(0)));
  publishParseDiagnostics({ParseDiagnostic::Warning(refusal.reason, refusal.range)}, std::nullopt);
}

void AsyncSVGDocument::applyInsertText(const EditorCommand& command) {
  if (!command.parentElement.has_value() || !command.element.has_value()) {
    return;
  }
  // Insert before setting text: projection initially serializes an empty element.
  svg::SVGElement textElement = *command.element;
  xml::ApplySourceEditResult result =
      document_->insertElement(*command.parentElement, textElement, command.referenceElement);
  lastFlushResult_.sourceDeltas.insert(lastFlushResult_.sourceDeltas.end(),
                                       result.sourceDeltas.begin(), result.sourceDeltas.end());
  if (result.diagnostic.has_value()) {
    publishParseDiagnostics({}, std::move(result.diagnostic));
  }
  if (textElement.isa<svg::SVGTextContentElement>()) {
    if (!command.textContent.empty()) {
      xml::ApplySourceEditResult textResult =
          document_->setElementTextContent(textElement, command.textContent);
      lastFlushResult_.sourceDeltas.insert(lastFlushResult_.sourceDeltas.end(),
                                           textResult.sourceDeltas.begin(),
                                           textResult.sourceDeltas.end());
      if (textResult.diagnostic.has_value() && !lastParseError_.has_value()) {
        publishParseDiagnostics({}, std::move(textResult.diagnostic));
      }
    }
    textElement.cast<svg::SVGTextContentElement>().setTextContent(command.textContent);
  }
}

void AsyncSVGDocument::applyOne(const EditorCommand& command) {
  switch (command.kind) {
    case EditorCommand::Kind::SetTransform: {
      if (!command.element.has_value()) {
        return;
      }
      // The element handle was captured by a public API (hitTest, tree
      // traversal, or querySelector) so we just cast to the graphics-
      // element interface and call the public transform setter - no
      // direct ECS access.
      svg::SVGGraphicsElement graphics =
          command.element->withReadAccess([&command](svg::DocumentReadAccess&, EntityHandle) {
            return command.element->cast<svg::SVGGraphicsElement>();
          });
      graphics.setTransform(command.transform);
      break;
    }

    case EditorCommand::Kind::ReplaceDocument: {
      // ReplaceDocument bypasses applyOne's mid-flush context: it always
      // produces a fresh document via the parser, so we re-enter the
      // setDocument path which clears the queue (already drained) and bumps
      // the version (which `flushFrame` will bump again, harmlessly).
      //
      // Every replacement may preserve entity identity when its parsed tree is structurally
      // equivalent. `preserveUndoOnReparse` controls undo metadata, not compositor invalidation.
      ParseWarningSink sink;
      auto result = svg::parser::SVGParser::ParseSVG(command.bytes, sink, EditorParseOptions());
      if (result.hasError()) {
        publishParseDiagnostics(CopyWarnings(sink), std::move(result.error()));
        return;
      }
      (void)setDocumentMaybeStructural(std::move(result).result());
      captureSourceRecoveryPolicy();
      publishParseDiagnostics(CopyWarnings(sink), std::nullopt);
      break;
    }

    case EditorCommand::Kind::SetAttribute: {
      if (!command.element.has_value()) {
        return;
      }
      // Apply the attribute change via the public SVGElement::setAttribute
      // API - the same path the parser uses. This triggers presentation-
      // attribute parsing, style cascade, and dirty-flag marking through
      // the same code path as initial parse. A structured-editing
      // fast path could eventually replace this with a targeted
      // AttributeParser::ParseAndSetAttribute call, but today the
      // public setAttribute is correct and sufficient.
      svg::SVGElement element = *command.element;
      xml::ApplySourceEditResult result = document_->setElementAttribute(
          element, xml::XMLQualifiedNameRef(command.attributeName), command.attributeValue);
      lastFlushResult_.sourceDeltas.insert(lastFlushResult_.sourceDeltas.end(),
                                           result.sourceDeltas.begin(), result.sourceDeltas.end());
      break;
    }

    case EditorCommand::Kind::RemoveAttribute: {
      if (!command.element.has_value()) {
        return;
      }
      // DOM-level attribute removal via the same structured-editing path as
      // SetAttribute, so the source reflection drops the attribute too.
      svg::SVGElement element = *command.element;
      xml::ApplySourceEditResult result = document_->removeElementAttribute(
          element, xml::XMLQualifiedNameRef(command.attributeName));
      lastFlushResult_.sourceDeltas.insert(lastFlushResult_.sourceDeltas.end(),
                                           result.sourceDeltas.begin(), result.sourceDeltas.end());
      break;
    }

    case EditorCommand::Kind::InsertElement: {
      if (!command.parentElement.has_value() || !command.element.has_value()) {
        return;
      }
      xml::ApplySourceEditResult result = document_->insertElement(
          *command.parentElement, *command.element, command.referenceElement);
      lastFlushResult_.sourceDeltas.insert(lastFlushResult_.sourceDeltas.end(),
                                           result.sourceDeltas.begin(), result.sourceDeltas.end());
      if (result.diagnostic.has_value()) {
        publishParseDiagnostics({}, std::move(result.diagnostic));
      }
      break;
    }

    case EditorCommand::Kind::ReplaceElements: {
      applyReplacementBatch(command);
      break;
    }

    case EditorCommand::Kind::DeleteElement: {
      if (!command.element.has_value()) {
        return;
      }
      // Detach via the public `SVGElement::remove()` method. The ECS
      // entity stays in the registry (orphaned, not garbage-collected)
      // so any in-flight references - stale selection, undo snapshots -
      // remain valid but invisible to the renderer because the tree
      // walk stops at the detach point. We copy to a local because
      // `remove()` is non-const and `command` is `const&`.
      svg::SVGElement element = *command.element;
      xml::ApplySourceEditResult result = document_->removeElement(element);
      lastFlushResult_.sourceDeltas.insert(lastFlushResult_.sourceDeltas.end(),
                                           result.sourceDeltas.begin(), result.sourceDeltas.end());
      break;
    }

    case EditorCommand::Kind::CutShapes:
    case EditorCommand::Kind::PasteShapes: {
      // Shape-clipboard structural replaces reparse `bytes` into a fresh
      // document, exactly like ReplaceDocument(preserveUndoOnReparse=true).
      // The kind discriminator is preserved at this layer purely for
      // labelling - undo entries are recorded by the orchestrator
      // (EditorShell), not here. On parse failure we leave the existing
      // document in place and surface a diagnostic.
      ParseWarningSink sink;
      auto result = svg::parser::SVGParser::ParseSVG(command.bytes, sink, EditorParseOptions());
      if (result.hasError()) {
        publishParseDiagnostics(CopyWarnings(sink), std::move(result.error()));
        return;
      }
      (void)setDocumentMaybeStructural(std::move(result).result());
      captureSourceRecoveryPolicy();
      publishParseDiagnostics(CopyWarnings(sink), std::nullopt);
      break;
    }

    case EditorCommand::Kind::InsertText: {
      applyInsertText(command);
      break;
    }

    case EditorCommand::Kind::SetTextContent: {
      if (!command.element.has_value()) {
        return;
      }
      svg::SVGElement textElement = *command.element;
      // Text-content edits are structural DOM edits: replace the element's
      // XML text node through the structured-editing path so source deltas
      // are emitted (the source pane mirrors them and a save persists them),
      // then update the component-level mirror so the renderer and inspector
      // reflect the new content immediately. `<style>` text lives in the
      // element's Data child nodes / parsed stylesheet rather than a
      // TextComponent, so it takes the SVGStyleElement path.
      xml::ApplySourceEditResult result =
          document_->setElementTextContent(textElement, command.textContent);
      lastFlushResult_.sourceDeltas.insert(lastFlushResult_.sourceDeltas.end(),
                                           result.sourceDeltas.begin(), result.sourceDeltas.end());
      if (result.diagnostic.has_value() && !lastParseError_.has_value()) {
        publishParseDiagnostics({}, std::move(result.diagnostic));
      }
      if (textElement.isa<svg::SVGTextContentElement>()) {
        textElement.cast<svg::SVGTextContentElement>().setTextContent(command.textContent);
      } else if (textElement.isa<svg::SVGStyleElement>()) {
        textElement.cast<svg::SVGStyleElement>().setTextContent(command.textContent);
      }
      break;
    }
  }
}

}  // namespace donner::editor
