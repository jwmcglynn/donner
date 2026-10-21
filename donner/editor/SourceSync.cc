#include "donner/editor/SourceSync.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "donner/base/FileOffset.h"
#include "donner/base/ParseResult.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/xml/XMLParser.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/EditorParseOptions.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::editor {

namespace {

struct SourceTextEdit {
  std::size_t offset = 0;
  std::size_t removedLength = 0;
  std::string_view replacement;
};

struct StructuredApplyResult {
  bool applied = false;
  bool needsDocumentReplace = false;
  bool recoveryBlocked = false;
  bool preserveUndoOnRecovery = false;
};

struct SelectionRemapTarget {
  svg::SVGElement element;
  AttributeWritebackTarget writebackTarget;
};

std::vector<SelectionRemapTarget> CaptureSelectionRemapTargets(const EditorApp& app) {
  std::vector<SelectionRemapTarget> targets;
  targets.reserve(app.selectedElements().size());
  for (const svg::SVGElement& element : app.selectedElements()) {
    if (std::optional<AttributeWritebackTarget> target = captureAttributeWritebackTarget(element);
        target.has_value()) {
      targets.push_back(SelectionRemapTarget{
          .element = element,
          .writebackTarget = std::move(*target),
      });
    }
  }

  return targets;
}

bool DocumentContainsElement(svg::SVGDocument& document, const svg::SVGElement& target) {
  return document.withReadAccess([&document, &target](svg::DocumentReadAccess&) {
    std::vector<svg::SVGElement> stack;
    stack.push_back(document.svgElement());
    while (!stack.empty()) {
      svg::SVGElement current = stack.back();
      stack.pop_back();
      if (current == target) {
        return true;
      }

      for (std::optional<svg::SVGElement> child = current.firstChild(); child.has_value();
           child = child->nextSibling()) {
        stack.push_back(*child);
      }
    }

    return false;
  });
}

std::optional<svg::SVGElement> ResolveSelectionTargetById(svg::SVGDocument& document,
                                                          const AttributeWritebackTarget& target) {
  return document.withReadAccess(
      [&document, &target](svg::DocumentReadAccess&) -> std::optional<svg::SVGElement> {
        if (!target.elementId.has_value() || target.elementPath.empty()) {
          return std::nullopt;
        }

        const auto targetTagName = target.elementPath.back().qualifiedName;
        std::vector<svg::SVGElement> stack;
        stack.push_back(document.svgElement());
        while (!stack.empty()) {
          svg::SVGElement current = stack.back();
          stack.pop_back();
          if (current.tagName() == targetTagName && current.id() == *target.elementId) {
            return current;
          }

          for (std::optional<svg::SVGElement> child = current.firstChild(); child.has_value();
               child = child->nextSibling()) {
            stack.push_back(*child);
          }
        }

        return std::nullopt;
      });
}

void RemapSelectionAfterStructuredSourceEdit(EditorApp& app,
                                             const std::vector<SelectionRemapTarget>& targets) {
  if (targets.empty() && app.selectedElements().empty()) {
    return;
  }

  std::vector<svg::SVGElement> remappedSelection;
  remappedSelection.reserve(targets.size());
  for (const SelectionRemapTarget& target : targets) {
    svg::SVGDocument& document = app.document().document();
    if (DocumentContainsElement(document, target.element)) {
      remappedSelection.push_back(target.element);
      continue;
    }

    std::optional<svg::SVGElement> element;
    if (target.writebackTarget.elementId.has_value()) {
      element = ResolveSelectionTargetById(document, target.writebackTarget);
    } else {
      element = resolveAttributeWritebackTarget(document, target.writebackTarget);
    }

    if (element.has_value()) {
      remappedSelection.push_back(*element);
    }
  }

  app.setSelection(std::move(remappedSelection));
}

std::optional<SourceTextEdit> BuildSingleSourceTextEdit(std::string_view oldSource,
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

  return SourceTextEdit{
      .offset = prefixLength,
      .removedLength = oldSource.size() - prefixLength - suffixLength,
      .replacement = newSource.substr(prefixLength, newSource.size() - prefixLength - suffixLength),
  };
}

std::optional<std::string> ApplySourceTextEdit(std::string_view source,
                                               const SourceEditIntent& intent) {
  if (intent.offset > source.size() || intent.removedLength > source.size() - intent.offset) {
    return std::nullopt;
  }

  std::string result(source);
  result.replace(intent.offset, intent.removedLength, intent.replacement);
  return result;
}

/// Default editor recovery must not broaden a source document's recorded parser policy or limits.
bool CanRecoverWithEditorParser(const std::optional<xml::XMLParser::Options>& sourceOptions) {
  const auto editorOptions = EditorParseOptions();
  xml::XMLParser::Options expected;
  expected.parseCustomEntities = true;
  expected.maximumInputSize = editorOptions.maximumInputSize;
  expected.maxElements = editorOptions.maximumTreeNodes;
  expected.maxNestingDepth = static_cast<int>(std::min(
      editorOptions.maximumTreeDepth, static_cast<std::size_t>(std::numeric_limits<int>::max())));
  return sourceOptions == expected;
}

/// Record document recovery only when the editor can preserve the admitted source policy.
StructuredApplyResult SourceEditRecoveryResult(svg::SVGDocument& document,
                                               xml::ReparseScope scope) {
  if (scope != xml::ReparseScope::Document) {
    return StructuredApplyResult{.applied = true};
  }
  return document.withReadAccess([&](svg::DocumentReadAccess&) {
    const auto sourceOptions = xml::XMLParser::SourceOptions(document.xmlDocument());
    return StructuredApplyResult{.applied = true,
                                 .needsDocumentReplace = true,
                                 .recoveryBlocked = !CanRecoverWithEditorParser(sourceOptions),
                                 .preserveUndoOnRecovery = true};
  });
}

/// Keep the last valid projection when a default editor parse would broaden the original policy.
bool QueueEditorSourceRecovery(EditorApp& app, std::string_view source, bool preserveUndo = true) {
  if (app.hasDocument() && !app.document().usesEditorParserForSourceRecovery()) {
    return false;
  }
  if (app.hasDocument() && !app.document().document().withReadAccess([&](svg::DocumentReadAccess&) {
        return CanRecoverWithEditorParser(
            xml::XMLParser::SourceOptions(app.document().document().xmlDocument()));
      })) {
    return false;
  }
  auto command = EditorCommand::ReplaceDocumentCommand(std::string(source), preserveUndo);
  if (app.hasDocument()) {
    command.sourceRecoveryState = app.document().sourceRecoveryState();
    if (!command.sourceRecoveryState) {
      return false;
    }
  }
  app.applyMutation(std::move(command));
  return true;
}

StructuredApplyResult TryApplyStructuredSourceEdit(EditorApp& app, std::string_view previousSource,
                                                   const SourceTextEdit& edit) {
  if (!app.structuredEditingEnabled() || !app.hasDocument()) {
    return {};
  }

  const std::vector<SelectionRemapTarget> selectionTargets = CaptureSelectionRemapTargets(app);

  svg::SVGDocument& document = app.document().document();
  if (!document.hasSourceStore() || document.source() != previousSource) {
    return {};
  }

  xml::ApplySourceEditResult result = app.document().applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(edit.offset),
                           FileOffset::Offset(edit.offset + edit.removedLength)},
      .replacement = edit.replacement,
      .sourceVersion = document.sourceVersion(),
  });

  if (!result.applied) {
    return {};
  }

  if (!result.diagnostic.has_value()) {
    RemapSelectionAfterStructuredSourceEdit(app, selectionTargets);
  }

  return SourceEditRecoveryResult(document, result.scope);
}

/// Append a structural fingerprint of \p element to \p out: a pre-order
/// `tag#id(children)` string. It is intentionally robust to formatting,
/// whitespace, and attribute *values* but sensitive to element
/// insert/delete/reorder/rename - exactly the desync an ambiguous whole-text
/// diff can introduce when it misclassifies a structural change as an attribute
/// edit.
void AppendStructuralFingerprint(const svg::SVGElement& element, std::string* out) {
  out->append(std::string(std::string_view(element.tagName().name)));
  out->push_back('#');
  if (std::optional<RcString> id = element.getAttribute(xml::XMLQualifiedNameRef("id"));
      id.has_value()) {
    out->append(id->str());
  }
  out->push_back('(');
  for (std::optional<svg::SVGElement> child = element.firstChild(); child.has_value();
       child = child->nextSibling()) {
    AppendStructuralFingerprint(*child, out);
  }
  out->push_back(')');
}

std::string StructuralFingerprint(const svg::SVGElement& root) {
  std::string fingerprint;
  AppendStructuralFingerprint(root, &fingerprint);
  return fingerprint;
}

/// Validate ambiguous whole-text changes only under the admitted editor parser policy.
/// Well-formed bytes may still leave stale tree shape or an unreconciled source span.
void ReconcileWholeTextSource(EditorApp& app, std::string_view newSource) {
  if (!app.document().usesEditorParserForSourceRecovery()) {
    return;
  }
  if (!app.document().document().withReadAccess([&](svg::DocumentReadAccess&) {
        return CanRecoverWithEditorParser(
            xml::XMLParser::SourceOptions(app.document().document().xmlDocument()));
      })) {
    return;
  }
  ParseWarningSink warningSink;
  ParseResult<svg::SVGDocument> freshParse =
      svg::parser::SVGParser::ParseSVG(newSource, warningSink, EditorParseOptions());
  if (freshParse.hasResult()) {
    const bool treeStale = app.document().document().xmlDocument().sourceDiagnostic().has_value() ||
                           StructuralFingerprint(app.document().document().svgElement()) !=
                               StructuralFingerprint(freshParse.result().svgElement());
    if (treeStale) {
      // A remount here is reconciliation for an ordinary keystroke, not a document load, so it
      // keeps the undo timeline the same way the writeback reparse below does.
      (void)QueueEditorSourceRecovery(app, newSource, /*preserveUndo=*/true);
    }
  }
}

bool TryApplyStructuredSourceChange(EditorApp& app, std::string_view previousSource,
                                    std::string_view newSource) {
  std::optional<SourceTextEdit> edit = BuildSingleSourceTextEdit(previousSource, newSource);
  if (!edit.has_value()) {
    return false;
  }

  const StructuredApplyResult result = TryApplyStructuredSourceEdit(app, previousSource, *edit);
  if (!result.applied) {
    return false;
  }

  if (result.recoveryBlocked ||
      (result.needsDocumentReplace && !app.document().usesEditorParserForSourceRecovery())) {
    return true;
  }
  if (result.needsDocumentReplace || app.document().document().source() != newSource) {
    (void)QueueEditorSourceRecovery(app, newSource, result.preserveUndoOnRecovery);
    return true;
  }

  ReconcileWholeTextSource(app, newSource);
  return true;
}

}  // namespace

void QueueSourceWritebackReparse(EditorApp& app, std::string_view newSource,
                                 std::string* previousSourceText,
                                 std::optional<std::string>* lastWritebackSourceText) {
  *previousSourceText = std::string(newSource);
  *lastWritebackSourceText = *previousSourceText;

  app.applyMutation(
      EditorCommand::ReplaceDocumentCommand(*previousSourceText, /*preserveUndoOnReparse=*/true));
}

DispatchSourceTextChangeResult DispatchSourceTextChange(
    EditorApp& app, std::string_view newSource, std::string* previousSourceText,
    std::optional<std::string>* lastWritebackSourceText) {
  if (lastWritebackSourceText->has_value() && newSource == **lastWritebackSourceText) {
    *previousSourceText = std::string(newSource);
    lastWritebackSourceText->reset();
    return DispatchSourceTextChangeResult{
        .dispatchedMutation = false,
        .skippedSelfWriteback = true,
    };
  }

  if (newSource == *previousSourceText) {
    return {};
  }

  bool handled = false;
  handled = TryApplyStructuredSourceChange(app, *previousSourceText, newSource);

  if (!handled) {
    (void)QueueEditorSourceRecovery(app, newSource);
  }

  *previousSourceText = std::string(newSource);
  lastWritebackSourceText->reset();
  return DispatchSourceTextChangeResult{
      .dispatchedMutation = true,
      .skippedSelfWriteback = false,
  };
}

/// Each source batch requests at most one full recovery; a blocked policy keeps its raw edits.
void ApplySourceIntentBatch(EditorApp& app, const std::vector<SourceEditIntent>& intents,
                            std::string_view newSource, std::string* workingSource) {
  for (const SourceEditIntent& intent : intents) {
    std::optional<std::string> nextSource = ApplySourceTextEdit(*workingSource, intent);
    if (!nextSource.has_value()) {
      (void)QueueEditorSourceRecovery(app, newSource);
      *workingSource = std::string(newSource);
      return;
    }

    const SourceTextEdit edit{
        .offset = intent.offset,
        .removedLength = intent.removedLength,
        .replacement = intent.replacement,
    };
    const StructuredApplyResult result = TryApplyStructuredSourceEdit(app, *workingSource, edit);
    if (result.recoveryBlocked ||
        (result.needsDocumentReplace && !app.document().usesEditorParserForSourceRecovery())) {
      *workingSource = std::move(*nextSource);
      continue;
    }
    if (!result.applied || result.needsDocumentReplace) {
      (void)QueueEditorSourceRecovery(app, newSource, result.preserveUndoOnRecovery);
      *workingSource = std::string(newSource);
      return;
    }

    if (app.document().document().source() != *nextSource) {
      (void)QueueEditorSourceRecovery(app, newSource);
      *workingSource = std::string(newSource);
      return;
    }

    *workingSource = std::move(*nextSource);
  }
}

DispatchSourceTextChangeResult DispatchSourceEditIntents(
    EditorApp& app, const std::vector<SourceEditIntent>& intents, std::string_view newSource,
    std::string* previousSourceText, std::optional<std::string>* lastWritebackSourceText) {
  if (lastWritebackSourceText->has_value() && newSource == **lastWritebackSourceText) {
    *previousSourceText = std::string(newSource);
    lastWritebackSourceText->reset();
    return DispatchSourceTextChangeResult{
        .dispatchedMutation = false,
        .skippedSelfWriteback = true,
    };
  }

  if (newSource == *previousSourceText) {
    return {};
  }

  if (intents.empty()) {
    return DispatchSourceTextChange(app, newSource, previousSourceText, lastWritebackSourceText);
  }

  std::string workingSource = *previousSourceText;
  ApplySourceIntentBatch(app, intents, newSource, &workingSource);

  if (workingSource != newSource) {
    (void)QueueEditorSourceRecovery(app, newSource);
  }

  *previousSourceText = std::string(newSource);
  lastWritebackSourceText->reset();
  return DispatchSourceTextChangeResult{
      .dispatchedMutation = true,
      .skippedSelfWriteback = false,
  };
}

}  // namespace donner::editor
