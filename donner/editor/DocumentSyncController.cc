#include "donner/editor/DocumentSyncController.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <string_view>
#include <vector>

#include "donner/editor/SelectTool.h"
#include "donner/editor/SourceSync.h"
#include "donner/editor/TextPatch.h"
#include "donner/editor/TransformSyntaxPreserving.h"

namespace donner::editor {

namespace {

constexpr float kTextChangeDebounceSeconds = 0.15f;

struct SourceMirrorEdit {
  std::size_t offset = 0;
  std::size_t removedLength = 0;
  std::string_view replacement;
};

enum class OffsetMappingBias {
  BeforeReplacement,
  AfterReplacement,
};

std::optional<SourceMirrorEdit> BuildSingleSourceMirrorEdit(std::string_view oldSource,
                                                            std::string_view newSource) {
  if (oldSource == newSource) {
    return std::nullopt;
  }

  std::size_t prefixLength = 0;
  const std::size_t commonLimit = std::min(oldSource.size(), newSource.size());
  while (prefixLength < commonLimit && oldSource[prefixLength] == newSource[prefixLength]) {
    ++prefixLength;
  }

  std::size_t suffixLength = 0;
  while (suffixLength < oldSource.size() - prefixLength &&
         suffixLength < newSource.size() - prefixLength &&
         oldSource[oldSource.size() - suffixLength - 1] ==
             newSource[newSource.size() - suffixLength - 1]) {
    ++suffixLength;
  }

  return SourceMirrorEdit{
      .offset = prefixLength,
      .removedLength = oldSource.size() - prefixLength - suffixLength,
      .replacement = newSource.substr(prefixLength, newSource.size() - prefixLength - suffixLength),
  };
}

std::optional<std::size_t> MapOffsetThroughSourceMirrorEdit(std::size_t offset,
                                                            const SourceMirrorEdit& edit,
                                                            OffsetMappingBias bias) {
  const std::size_t editEnd = edit.offset + edit.removedLength;
  if (editEnd < edit.offset) {
    return std::nullopt;
  }

  if (offset < edit.offset) {
    return offset;
  }

  if (offset > editEnd) {
    return offset - edit.removedLength + edit.replacement.size();
  }

  if (offset == edit.offset && bias == OffsetMappingBias::BeforeReplacement) {
    return edit.offset;
  }

  return edit.offset + edit.replacement.size();
}

bool SourceRangeConflictsWithMirrorEdit(std::size_t offset, std::size_t length,
                                        const SourceMirrorEdit& edit) {
  const std::size_t end = offset + length;
  const std::size_t editEnd = edit.offset + edit.removedLength;
  if (end < offset || editEnd < edit.offset) {
    return true;
  }

  if (length == 0) {
    return offset > edit.offset && offset < editEnd;
  }

  if (edit.removedLength == 0) {
    return offset <= edit.offset && edit.offset < end;
  }

  return offset < editEnd && edit.offset < end;
}

std::optional<std::size_t> MapOffsetByFollowingContext(std::string_view oldSource,
                                                       std::string_view newSource,
                                                       std::size_t offset) {
  if (offset >= oldSource.size()) {
    return std::nullopt;
  }

  constexpr std::size_t kMaxContextLength = 64;
  const std::size_t maxContextLength = std::min(kMaxContextLength, oldSource.size() - offset);
  for (std::size_t contextLength = 1; contextLength <= maxContextLength; ++contextLength) {
    const std::string_view context = oldSource.substr(offset, contextLength);
    const std::size_t firstMatch = newSource.find(context);
    if (firstMatch == std::string_view::npos) {
      continue;
    }

    if (newSource.find(context, firstMatch + 1) == std::string_view::npos) {
      return firstMatch;
    }
  }

  return std::nullopt;
}

std::optional<std::size_t> FindSvgRootCloseTag(std::string_view source) {
  const std::size_t closeOffset = source.rfind("</svg>");
  if (closeOffset == std::string_view::npos) {
    return std::nullopt;
  }
  return closeOffset;
}

bool LooksLikeElementInsertion(std::string_view replacement) {
  const std::size_t firstNonWhitespace = replacement.find_first_not_of(" \t\r\n");
  if (firstNonWhitespace == std::string_view::npos ||
      firstNonWhitespace + 1u >= replacement.size()) {
    return false;
  }

  return replacement[firstNonWhitespace] == '<' && replacement[firstNonWhitespace + 1u] != '/' &&
         replacement[firstNonWhitespace + 1u] != '!' && replacement[firstNonWhitespace + 1u] != '?';
}

std::optional<std::size_t> RemapRootChildInsertionToCurrentCloseTag(
    std::string_view baselineSource, std::string_view workingText, const xml::XMLSourceDelta& delta,
    std::string_view replacement, std::size_t mappedOffset) {
  if (delta.removedLength != 0 || !LooksLikeElementInsertion(replacement)) {
    return std::nullopt;
  }

  const std::optional<std::size_t> baselineCloseOffset = FindSvgRootCloseTag(baselineSource);
  const std::optional<std::size_t> workingCloseOffset = FindSvgRootCloseTag(workingText);
  if (!baselineCloseOffset.has_value() || !workingCloseOffset.has_value() ||
      delta.offset != *baselineCloseOffset || mappedOffset <= *workingCloseOffset) {
    return std::nullopt;
  }

  return *workingCloseOffset;
}

std::string CanonicalizeForTextEditor(std::string_view source) {
  std::string result(source);
  if (!result.empty() && result.back() == '\n') {
    result.pop_back();
  }
  return result;
}

void MarkMirroredDocumentSource(EditorApp& app, std::string_view source,
                                std::string* previousSourceText,
                                std::optional<std::string>* lastWritebackSourceText) {
  previousSourceText->assign(source);
  *lastWritebackSourceText = *previousSourceText;
  app.syncDirtyFromSource(source);
}

bool MirrorDocumentSourceIntoTextEditor(EditorApp& app, TextEditor& textEditor,
                                        std::string* previousSourceText,
                                        std::optional<std::string>* lastWritebackSourceText) {
  if (!app.hasDocument() || !app.document().document().hasSourceStore()) {
    return false;
  }

  std::string source = CanonicalizeForTextEditor(app.document().document().source());
  const std::string currentText = textEditor.getText();
  if (currentText != source) {
    if (std::optional<SourceMirrorEdit> edit = BuildSingleSourceMirrorEdit(currentText, source)) {
      textEditor.applyExternalSourceEdit(edit->offset, edit->removedLength, edit->replacement);
    }

    if (textEditor.getText() != source) {
      textEditor.setText(source, /*preserveScroll=*/true);
      textEditor.resetTextChanged();
    }
  }
  MarkMirroredDocumentSource(app, source, previousSourceText, lastWritebackSourceText);
  return true;
}

bool ApplyXMLSourceDeltasIntoTextEditor(EditorApp& app, TextEditor& textEditor,
                                        const std::vector<xml::XMLSourceDelta>& sourceDeltas,
                                        std::string* previousSourceText,
                                        std::optional<std::string>* lastWritebackSourceText) {
  if (!app.hasDocument() || !app.document().document().hasSourceStore() || sourceDeltas.empty()) {
    return false;
  }

  const std::string source = CanonicalizeForTextEditor(app.document().document().source());
  std::string baselineSource = *previousSourceText;
  std::string workingText = textEditor.getText();
  for (const xml::XMLSourceDelta& delta : sourceDeltas) {
    if (delta.offset > baselineSource.size() ||
        delta.removedLength > baselineSource.size() - delta.offset ||
        delta.offset > source.size() || delta.insertedLength > source.size() - delta.offset) {
      return MirrorDocumentSourceIntoTextEditor(app, textEditor, previousSourceText,
                                                lastWritebackSourceText);
    }

    const std::string_view replacement =
        std::string_view(source).substr(delta.offset, delta.insertedLength);
    std::size_t mappedOffset = delta.offset;
    std::size_t mappedRemovedLength = delta.removedLength;
    if (std::optional<SourceMirrorEdit> edit =
            BuildSingleSourceMirrorEdit(baselineSource, workingText);
        edit.has_value()) {
      if (SourceRangeConflictsWithMirrorEdit(delta.offset, delta.removedLength, *edit)) {
        if (delta.removedLength != 0) {
          return MirrorDocumentSourceIntoTextEditor(app, textEditor, previousSourceText,
                                                    lastWritebackSourceText);
        }

        std::optional<std::size_t> contextOffset =
            MapOffsetByFollowingContext(baselineSource, workingText, delta.offset);
        if (!contextOffset.has_value()) {
          return MirrorDocumentSourceIntoTextEditor(app, textEditor, previousSourceText,
                                                    lastWritebackSourceText);
        }

        mappedOffset = *contextOffset;
        if (std::optional<std::size_t> rootCloseOffset = RemapRootChildInsertionToCurrentCloseTag(
                baselineSource, workingText, delta, replacement, mappedOffset);
            rootCloseOffset.has_value()) {
          mappedOffset = *rootCloseOffset;
        }
        mappedRemovedLength = 0;
      } else {
        std::optional<std::size_t> mappedStart = MapOffsetThroughSourceMirrorEdit(
            delta.offset, *edit, OffsetMappingBias::AfterReplacement);
        std::optional<std::size_t> mappedEnd = MapOffsetThroughSourceMirrorEdit(
            delta.offset + delta.removedLength, *edit, OffsetMappingBias::AfterReplacement);
        if (!mappedStart.has_value() || !mappedEnd.has_value() || *mappedEnd < *mappedStart) {
          return MirrorDocumentSourceIntoTextEditor(app, textEditor, previousSourceText,
                                                    lastWritebackSourceText);
        }

        mappedOffset = *mappedStart;
        mappedRemovedLength = *mappedEnd - *mappedStart;
        if (std::optional<std::size_t> rootCloseOffset = RemapRootChildInsertionToCurrentCloseTag(
                baselineSource, workingText, delta, replacement, mappedOffset);
            rootCloseOffset.has_value()) {
          mappedOffset = *rootCloseOffset;
        }
      }
    }

    if (mappedOffset > workingText.size() ||
        mappedRemovedLength > workingText.size() - mappedOffset) {
      return MirrorDocumentSourceIntoTextEditor(app, textEditor, previousSourceText,
                                                lastWritebackSourceText);
    }

    textEditor.applyExternalSourceEdit(mappedOffset, mappedRemovedLength, replacement);
    workingText.replace(mappedOffset, mappedRemovedLength, replacement.data(), replacement.size());
    baselineSource.replace(delta.offset, delta.removedLength, replacement.data(),
                           replacement.size());
  }

  if (baselineSource != source) {
    return MirrorDocumentSourceIntoTextEditor(app, textEditor, previousSourceText,
                                              lastWritebackSourceText);
  }

  if (workingText != source) {
    QueueSourceWritebackReparse(app, workingText, previousSourceText, lastWritebackSourceText);
    return true;
  }

  MarkMirroredDocumentSource(app, source, previousSourceText, lastWritebackSourceText);
  return true;
}

}  // namespace

DocumentSyncController::DocumentSyncController(std::string initialSource)
    : previousSourceText_(std::move(initialSource)) {}

void DocumentSyncController::resetForLoadedDocument(const std::string& source) {
  previousSourceText_ = source;
  lastWritebackSourceText_.reset();
  pendingTransformWritebacks_.clear();
  pendingElementRemoveWritebacks_.clear();
  pendingSourceEditIntents_.clear();
  sourceDiagnostics_ = {};
  lastSyncedDiagnosticsRevision_ = std::numeric_limits<std::uint64_t>::max();
  textChangePending_ = false;
  textDispatchThrottled_ = false;
  textChangeIdleTimer_ = 0.0f;
}

void DocumentSyncController::syncParseErrorMarkers(EditorApp& app, TextEditor& textEditor) {
  const std::uint64_t revision = app.document().parseDiagnosticsRevision();
  if (revision == lastSyncedDiagnosticsRevision_) {
    return;
  }

  sourceDiagnostics_ = BuildSourceDiagnosticSnapshot(app.document().parseDiagnostics(),
                                                     textEditor.getText(), revision);
  lastSyncedDiagnosticsRevision_ = revision;
  textEditor.setSourceDiagnostics(sourceDiagnostics_.diagnostics);

  TextEditor::ErrorMarkers markers;
  for (const SourceDiagnostic& diagnostic : sourceDiagnostics_.diagnostics) {
    if (diagnostic.severity != DiagnosticSeverity::Error) {
      continue;
    }

    std::string& message = markers[static_cast<int>(diagnostic.line)];
    if (!message.empty()) {
      message += '\n';
    }
    message += diagnostic.message;
  }
  textEditor.setErrorMarkers(markers);
}

bool DocumentSyncController::mirrorSourceDeltas(
    EditorApp& app, TextEditor& textEditor, const std::vector<xml::XMLSourceDelta>& sourceDeltas) {
  return ApplyXMLSourceDeltasIntoTextEditor(app, textEditor, sourceDeltas, &previousSourceText_,
                                            &lastWritebackSourceText_);
}

void DocumentSyncController::handleTextEdits(EditorApp& app, TextEditor& textEditor,
                                             float deltaSeconds) {
  const auto dispatchTextChange = [&](std::string_view newSource) {
    if (pendingSourceEditIntents_.empty()) {
      (void)DispatchSourceTextChange(app, newSource, &previousSourceText_,
                                     &lastWritebackSourceText_);
      return;
    }

    (void)DispatchSourceEditIntents(app, pendingSourceEditIntents_, newSource, &previousSourceText_,
                                    &lastWritebackSourceText_);
    pendingSourceEditIntents_.clear();
  };

  if (textEditor.isTextChanged()) {
    const std::string newSource = textEditor.getText();
    std::vector<SourceEditIntent> editIntents = textEditor.takePendingSourceEditIntents();
    pendingSourceEditIntents_.insert(pendingSourceEditIntents_.end(),
                                     std::make_move_iterator(editIntents.begin()),
                                     std::make_move_iterator(editIntents.end()));
    app.syncDirtyFromSource(newSource);
    textEditor.resetTextChanged();

    if (!textDispatchThrottled_) {
      dispatchTextChange(newSource);
      textDispatchThrottled_ = true;
      textChangePending_ = false;
    } else {
      textChangePending_ = true;
    }
    textChangeIdleTimer_ = 0.0f;
  } else if (textDispatchThrottled_) {
    textChangeIdleTimer_ += deltaSeconds;
    if (textChangeIdleTimer_ >= kTextChangeDebounceSeconds) {
      if (textChangePending_) {
        const std::string newSource = textEditor.getText();
        app.syncDirtyFromSource(newSource);
        dispatchTextChange(newSource);
        textChangePending_ = false;
      }
      textDispatchThrottled_ = false;
      textChangeIdleTimer_ = 0.0f;
    }
  }
}

std::optional<float> DocumentSyncController::nextTextSyncWakeSeconds() const {
  if (!textDispatchThrottled_) {
    return std::nullopt;
  }

  return std::max(0.0f, kTextChangeDebounceSeconds - textChangeIdleTimer_);
}

void DocumentSyncController::applyPendingWritebacks(EditorApp& app, SelectTool& selectTool,
                                                    TextEditor& textEditor) {
  const AsyncSVGDocument::FlushResult& lastFlush = app.document().lastFlushResult();
  bool handledLastFlushSourceDeltas = false;
  if (lastFlush.replacedDocument && lastFlush.preserveUndoOnReparse && app.hasDocument() &&
      !app.document().lastParseError().has_value()) {
    (void)MirrorDocumentSourceIntoTextEditor(app, textEditor, &previousSourceText_,
                                             &lastWritebackSourceText_);
  }

  if (auto completed = selectTool.consumeCompletedDragWriteback(); completed.has_value()) {
    pendingTransformWritebacks_.push_back(EditorApp::CompletedTransformWriteback{
        .target = std::move(completed->target),
        .transform = completed->transform,
        .sourceTransformAttributeValue = std::move(completed->sourceTransformAttributeValue),
    });
    for (auto& extra : completed->extras) {
      pendingTransformWritebacks_.push_back(EditorApp::CompletedTransformWriteback{
          .target = std::move(extra.target),
          .transform = extra.transform,
          .sourceTransformAttributeValue = std::move(extra.sourceTransformAttributeValue),
      });
    }
  }
  for (auto& completed : app.consumeTransformWritebacks()) {
    pendingTransformWritebacks_.push_back(std::move(completed));
  }

  auto completedRemovals = app.consumeElementRemoveWritebacks();
  for (auto& writeback : completedRemovals) {
    pendingElementRemoveWritebacks_.push_back(std::move(writeback));
  }

  if (!pendingElementRemoveWritebacks_.empty()) {
    handledLastFlushSourceDeltas = true;
    if (mirrorSourceDeltas(app, textEditor, app.document().lastFlushResult().sourceDeltas) ||
        MirrorDocumentSourceIntoTextEditor(app, textEditor, &previousSourceText_,
                                           &lastWritebackSourceText_)) {
      pendingElementRemoveWritebacks_.clear();
    } else {
      std::string source = textEditor.getText();
      bool changed = false;
      for (const auto& pendingRemove : pendingElementRemoveWritebacks_) {
        auto patch = buildElementRemoveWriteback(source, pendingRemove.target);
        if (!patch.has_value()) {
          continue;
        }

        applyPatches(source, {{*patch}});
        changed = true;
      }

      pendingElementRemoveWritebacks_.clear();
      if (changed) {
        textEditor.setText(source, /*preserveScroll=*/true);
        QueueSourceWritebackReparse(app, source, &previousSourceText_, &lastWritebackSourceText_);
      }
    }
  }

  if (!handledLastFlushSourceDeltas && !lastFlush.sourceDeltas.empty()) {
    if (!mirrorSourceDeltas(app, textEditor, lastFlush.sourceDeltas)) {
      (void)MirrorDocumentSourceIntoTextEditor(app, textEditor, &previousSourceText_,
                                               &lastWritebackSourceText_);
    }
  }

  if (pendingTransformWritebacks_.empty()) {
    return;
  }

  if (app.hasDocument() && app.document().document().hasSourceStore()) {
    svg::SVGDocument& document = app.document().document();
    bool mirroredSourceDeltas = false;
    for (const auto& writeback : pendingTransformWritebacks_) {
      std::optional<svg::SVGElement> element =
          resolveAttributeWritebackTarget(document, writeback.target);
      if (!element.has_value()) {
        continue;
      }

      xml::ApplySourceEditResult result;
      if (writeback.restoreSourceTransformAttributeValue) {
        if (writeback.sourceTransformAttributeValue.has_value()) {
          result = document.setElementAttribute(*element, "transform",
                                                *writeback.sourceTransformAttributeValue);
        } else {
          result = document.removeElementAttribute(*element, "transform");
        }
      } else {
        const RcString serialized = serializeTransformForWriteback(
            writeback.sourceTransformAttributeValue, writeback.transform);
        if (std::string_view(serialized).empty()) {
          result = document.removeElementAttribute(*element, "transform");
        } else {
          result = document.setElementAttribute(*element, "transform", serialized);
        }
      }

      mirroredSourceDeltas =
          mirrorSourceDeltas(app, textEditor, result.sourceDeltas) || mirroredSourceDeltas;
    }

    pendingTransformWritebacks_.clear();
    if (!mirroredSourceDeltas) {
      (void)MirrorDocumentSourceIntoTextEditor(app, textEditor, &previousSourceText_,
                                               &lastWritebackSourceText_);
    }
    return;
  }

  std::string source = textEditor.getText();
  std::vector<TextPatch> patches;
  patches.reserve(pendingTransformWritebacks_.size());
  for (const auto& writeback : pendingTransformWritebacks_) {
    std::optional<TextPatch> patch;
    if (writeback.restoreSourceTransformAttributeValue) {
      if (writeback.sourceTransformAttributeValue.has_value()) {
        patch = buildAttributeWriteback(source, writeback.target, "transform",
                                        std::string_view(*writeback.sourceTransformAttributeValue));
      } else {
        patch = buildAttributeRemoveWriteback(source, writeback.target, "transform");
      }
    } else {
      const RcString serialized = serializeTransformForWriteback(
          writeback.sourceTransformAttributeValue, writeback.transform);
      if (std::string_view(serialized).empty()) {
        patch = buildAttributeRemoveWriteback(source, writeback.target, "transform");
      } else {
        patch = buildAttributeWriteback(source, writeback.target, "transform",
                                        std::string_view(serialized));
      }
    }
    if (patch.has_value()) {
      patches.push_back(*std::move(patch));
    }
  }
  pendingTransformWritebacks_.clear();
  if (patches.empty()) {
    return;
  }

  applyPatches(source, patches);
  textEditor.setText(source, /*preserveScroll=*/true);
  QueueSourceWritebackReparse(app, source, &previousSourceText_, &lastWritebackSourceText_);
}

}  // namespace donner::editor
