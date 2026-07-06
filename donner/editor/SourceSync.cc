#include "donner/editor/SourceSync.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "donner/base/FileOffset.h"
#include "donner/base/ParseResult.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/EditorCommand.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::editor {

namespace {

/// Count of full-document reparse fallbacks triggered by the structural-fingerprint mismatch
/// check in \ref TryApplyStructuredSourceChange (see that function for why the fallback exists).
/// Atomic defensively - this file's callers are expected to run on the editor's main thread, but
/// nothing here enforces that. Exposed read-only via \ref FingerprintFallbackCountForTesting so
/// the fallback rate is observable (this path is O(document): it reparses the whole source and
/// walks both resulting trees, so how often it fires in practice matters).
std::atomic<std::uint64_t> g_fingerprintFallbackCount{0};

struct SourceTextEdit {
  std::size_t offset = 0;
  std::size_t removedLength = 0;
  std::string_view replacement;
};

struct StructuredApplyResult {
  bool applied = false;
  bool needsDocumentReplace = false;
  bool hadDiagnostic = false;
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

  return StructuredApplyResult{
      .applied = true,
      .needsDocumentReplace = result.scope == xml::ReparseScope::Document,
      .hadDiagnostic = result.diagnostic.has_value(),
  };
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

  if (result.needsDocumentReplace || app.document().document().source() != newSource) {
    app.applyMutation(EditorCommand::ReplaceDocumentCommand(std::string(newSource)));
    return true;
  }

  // Guard against a lossy incremental apply. `BuildSingleSourceTextEdit` produces
  // a minimal whole-text diff, which is ambiguous: inserting an element textually
  // similar to a sibling can collapse into what looks like a single-character
  // attribute edit, so the structured apply renames the existing sibling and
  // never materializes the new element even though the source bytes are correct.
  // Skip the check when the apply carried a diagnostic - the source is then
  // intentionally mid-error and the editor keeps the last-good DOM. Otherwise
  // compare a structural fingerprint of the live DOM against a fresh parse of the
  // new source; on mismatch the DOM desynced, so fall back to a full reparse.
  //
  // That fallback is O(document): it reparses the entire new source and walks
  // both the live and freshly-parsed trees to build their fingerprints, on
  // every incremental apply this check runs for (not just the ones that end up
  // desynced). `g_fingerprintFallbackCount` and the log line below make how
  // often it actually fires observable rather than silent.
  if (!result.hadDiagnostic) {
    ParseWarningSink warningSink;
    ParseResult<svg::SVGDocument> freshParse =
        svg::parser::SVGParser::ParseSVG(newSource, warningSink);
    if (freshParse.hasResult() && StructuralFingerprint(app.document().document().svgElement()) !=
                                      StructuralFingerprint(freshParse.result().svgElement())) {
      const std::uint64_t fallbackCount =
          g_fingerprintFallbackCount.fetch_add(1, std::memory_order_relaxed) + 1;
      std::fprintf(stderr,
                   "[SourceSync] fingerprint fallback #%" PRIu64 ": incremental apply desynced "
                   "from a fresh parse, falling back to a full-document reparse\n",
                   fallbackCount);
      app.applyMutation(EditorCommand::ReplaceDocumentCommand(std::string(newSource)));
    }
  }

  return true;
}

}  // namespace

std::uint64_t FingerprintFallbackCountForTesting() {
  return g_fingerprintFallbackCount.load(std::memory_order_relaxed);
}

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
    app.applyMutation(EditorCommand::ReplaceDocumentCommand(std::string(newSource)));
  }

  *previousSourceText = std::string(newSource);
  lastWritebackSourceText->reset();
  return DispatchSourceTextChangeResult{
      .dispatchedMutation = true,
      .skippedSelfWriteback = false,
  };
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
  for (const SourceEditIntent& intent : intents) {
    std::optional<std::string> nextSource = ApplySourceTextEdit(workingSource, intent);
    if (!nextSource.has_value()) {
      app.applyMutation(EditorCommand::ReplaceDocumentCommand(std::string(newSource)));
      *previousSourceText = std::string(newSource);
      lastWritebackSourceText->reset();
      return DispatchSourceTextChangeResult{
          .dispatchedMutation = true,
          .skippedSelfWriteback = false,
      };
    }

    const SourceTextEdit edit{
        .offset = intent.offset,
        .removedLength = intent.removedLength,
        .replacement = intent.replacement,
    };
    const StructuredApplyResult result = TryApplyStructuredSourceEdit(app, workingSource, edit);
    if (!result.applied || result.needsDocumentReplace) {
      app.applyMutation(EditorCommand::ReplaceDocumentCommand(std::string(newSource)));
      *previousSourceText = std::string(newSource);
      lastWritebackSourceText->reset();
      return DispatchSourceTextChangeResult{
          .dispatchedMutation = true,
          .skippedSelfWriteback = false,
      };
    }

    if (app.document().document().source() != *nextSource) {
      app.applyMutation(EditorCommand::ReplaceDocumentCommand(std::string(newSource)));
      *previousSourceText = std::string(newSource);
      lastWritebackSourceText->reset();
      return DispatchSourceTextChangeResult{
          .dispatchedMutation = true,
          .skippedSelfWriteback = false,
      };
    }

    workingSource = std::move(*nextSource);
  }

  if (workingSource != newSource) {
    app.applyMutation(EditorCommand::ReplaceDocumentCommand(std::string(newSource)));
  }

  *previousSourceText = std::string(newSource);
  lastWritebackSourceText->reset();
  return DispatchSourceTextChangeResult{
      .dispatchedMutation = true,
      .skippedSelfWriteback = false,
  };
}

}  // namespace donner::editor
