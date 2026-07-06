/// @file EditorStateMachine_fuzzer.cc
///
/// Headless libFuzzer target for editor interaction state. It decodes bytes
/// into source-editor and canvas-selection actions, then probes the same
/// source sync, focus, undo, selection, and render paths used by the GUI
/// without creating an ImGui context or a native window.

#include <fuzzer/FuzzedDataProvider.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/FileOffset.h"
#include "donner/base/Utils.h"
#include "donner/base/Vector2.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/FocusView.h"
#include "donner/editor/SelectionAabb.h"
#include "donner/editor/SourceSync.h"
#include "donner/editor/TextEditorCore.h"
#include "donner/editor/TextPatch.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/renderer/Renderer.h"

namespace donner::editor {
namespace {

constexpr std::size_t kMaxInputSize = 8192;
constexpr std::size_t kMaxSourceSize = 4096;
constexpr int kMaxSteps = 96;

constexpr std::string_view kStyledRefsSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="180" height="100" viewBox="0 0 180 100">
  <style>
    .hit { fill: url(#paint); filter: url(#blur); }
    .accent, #sibling { stroke: #224466; stroke-width: 2; }
  </style>
  <defs>
    <linearGradient id="paint"><stop offset="0" stop-color="red"/><stop offset="1" stop-color="blue"/></linearGradient>
    <filter id="blur"><feGaussianBlur stdDeviation="1.5"/></filter>
  </defs>
  <g id="layer" transform="translate(0 0)">
    <rect id="target" class="hit accent" x="12" y="10" width="42" height="28"/>
    <circle id="sibling" class="hit" cx="94" cy="28" r="18"/>
  </g>
</svg>)svg";

constexpr std::string_view kNestedGroupsSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="180" height="100" viewBox="0 0 180 100">
  <defs>
    <clipPath id="clip"><rect x="70" y="10" width="70" height="70"/></clipPath>
    <radialGradient id="glow"><stop offset="0" stop-color="white"/><stop offset="1" stop-color="orange"/></radialGradient>
  </defs>
  <g id="outer" clip-path="url(#clip)" opacity="0.8">
    <g id="inner" transform="rotate(0)">
      <path id="boltA" class="bright" d="M80 12 L122 40 L98 40 L132 82 L86 50 L108 50 Z" fill="url(#glow)"/>
      <path id="boltB" d="M20 70 L44 30 L68 70 Z" fill="green"/>
    </g>
  </g>
</svg>)svg";

constexpr std::string_view kMalformedRecoverySvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="180" height="100">
  <style>
    * { opacity: 0.95; }
    rect.focused { fill: purple; }
  </style>
  <rect id="focused" class="focused" x="20" y="20" width="50" height="40"/>
  <text id="label" x="20" y="82">Hello</text>
</svg>)svg";

constexpr std::array<std::string_view, 3> kSeedSources = {
    kStyledRefsSvg,
    kNestedGroupsSvg,
    kMalformedRecoverySvg,
};

constexpr std::array<std::string_view, 8> kKnownIds = {
    "target", "sibling", "layer", "outer", "inner", "boltA", "focused", "label",
};

constexpr std::array<std::string_view, 14> kReplacementSnippets = {
    "",
    " ",
    R"xml( fill="red")xml",
    R"xml( class="hit")xml",
    R"xml( transform="translate(1 2)")xml",
    R"xml( filter="url(#blur)")xml",
    R"xml( width="17")xml",
    R"xml( d="M0 0 L10 0 L5 8 Z")xml",
    R"xml(<g id="typed"><rect width="8" height="9"/></g>)xml",
    R"xml(<rect id="newRect" class="hit" x="4" y="5" width="12" height="13"/>)xml",
    "</g>",
    R"xml(")xml",
    R"xml(<style>.typed { fill: url(#paint); }</style>)xml",
    R"xml( style="fill: url(#paint); stroke: blue")xml",
};

std::size_t ConsumeOffset(FuzzedDataProvider& provider, std::string_view text) {
  if (text.empty()) {
    return 0;
  }

  return provider.ConsumeIntegralInRange<std::size_t>(0, text.size());
}

std::string ConsumeXmlishString(FuzzedDataProvider& provider) {
  constexpr std::string_view kAlphabet =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
      " \n\t<>/=\"'#:;.-_(){}%,";
  const std::size_t length = provider.ConsumeIntegralInRange<std::size_t>(0, 32);
  std::string result;
  result.reserve(length);
  for (std::size_t i = 0; i < length; ++i) {
    result.push_back(
        kAlphabet[provider.ConsumeIntegralInRange<std::size_t>(0, kAlphabet.size() - 1)]);
  }
  return result;
}

std::string ConsumeReplacement(FuzzedDataProvider& provider) {
  if (provider.ConsumeBool()) {
    return std::string(provider.PickValueInArray(kReplacementSnippets));
  }

  return ConsumeXmlishString(provider);
}

Vector2d ConsumeDocumentPoint(FuzzedDataProvider& provider) {
  return Vector2d(provider.ConsumeFloatingPointInRange<double>(-20.0, 220.0),
                  provider.ConsumeFloatingPointInRange<double>(-20.0, 140.0));
}

std::optional<std::size_t> ResolveFileOffset(std::string_view source, const FileOffset& offset) {
  const FileOffset resolved = offset.resolveOffset(source);
  if (!resolved.offset.has_value()) {
    return std::nullopt;
  }

  return std::min(*resolved.offset, source.size());
}

bool RangeContainsOffset(std::string_view source, const SourceRange& range, std::size_t offset) {
  const std::optional<std::size_t> start = ResolveFileOffset(source, range.start);
  const std::optional<std::size_t> end = ResolveFileOffset(source, range.end);
  return start.has_value() && end.has_value() && *start <= offset && offset < *end;
}

std::optional<svg::SVGElement> FindElementAtSourceOffsetImpl(const svg::SVGElement& element,
                                                             std::string_view source,
                                                             std::size_t offset) {
  const std::optional<xml::XMLNode> xmlNode = xml::XMLNode::TryCast(element.entityHandle());
  if (!xmlNode.has_value()) {
    return std::nullopt;
  }

  const std::optional<SourceRange> range = xmlNode->getNodeLocation();
  if (!range.has_value() || !RangeContainsOffset(source, *range, offset)) {
    return std::nullopt;
  }

  for (std::optional<svg::SVGElement> child = element.firstChild(); child.has_value();
       child = child->nextSibling()) {
    std::optional<svg::SVGElement> childMatch =
        FindElementAtSourceOffsetImpl(*child, source, offset);
    if (childMatch.has_value()) {
      return childMatch;
    }
  }

  return element;
}

bool IsAncestorOrSelf(const svg::SVGElement& maybeAncestor, const svg::SVGElement& element) {
  for (std::optional<svg::SVGElement> current = element; current.has_value();
       current = current->parentElement()) {
    if (*current == maybeAncestor) {
      return true;
    }
  }

  return false;
}

bool RangeEndsAt(std::string_view source, const std::optional<SourceRange>& range,
                 std::size_t offset) {
  if (!range.has_value()) {
    return false;
  }

  const std::optional<std::size_t> end = ResolveFileOffset(source, range->end);
  return end.has_value() && *end == offset;
}

bool ElementTagEndsAt(const svg::SVGElement& element, std::string_view source, std::size_t offset) {
  const std::optional<xml::XMLNode> xmlNode = xml::XMLNode::TryCast(element.entityHandle());
  return xmlNode.has_value() && (RangeEndsAt(source, xmlNode->getOpeningTagLocation(), offset) ||
                                 RangeEndsAt(source, xmlNode->getClosingTagLocation(), offset));
}

std::optional<svg::SVGElement> FindElementAtSourceOffset(const svg::SVGDocument& document,
                                                         std::string_view source,
                                                         std::size_t offset) {
  if (offset >= source.size()) {
    return std::nullopt;
  }

  return FindElementAtSourceOffsetImpl(document.svgElement(), source, offset);
}

std::optional<svg::SVGElement> FindElementNearSourceOffset(const svg::SVGDocument& document,
                                                           std::string_view source,
                                                           std::size_t offset) {
  std::optional<svg::SVGElement> current =
      offset < source.size() ? FindElementAtSourceOffset(document, source, offset) : std::nullopt;
  if (offset == 0) {
    return current;
  }

  std::optional<svg::SVGElement> previous = FindElementAtSourceOffset(document, source, offset - 1);
  if (previous.has_value() && ElementTagEndsAt(*previous, source, offset)) {
    return previous;
  }

  if (previous.has_value() && (!current.has_value() || IsAncestorOrSelf(*current, *previous))) {
    return previous;
  }

  return current;
}

bool IsElementInCurrentDocument(const EditorApp& app, const svg::SVGElement& element) {
  if (!app.hasDocument()) {
    return false;
  }

  const EntityHandle handle = element.entityHandle();
  if (handle.registry() != &app.document().document().registry()) {
    return false;
  }

  return static_cast<bool>(handle);
}

class HeadlessEditorFuzzSession {
public:
  explicit HeadlessEditorFuzzSession(FuzzedDataProvider& provider) : provider_(provider) {
    LoadSource(provider_.PickValueInArray(kSeedSources));
  }

  void Run() {
    const int steps = provider_.ConsumeIntegralInRange<int>(0, kMaxSteps);
    for (int i = 0; i < steps && provider_.remaining_bytes() > 0; ++i) {
      RunAction(provider_.ConsumeIntegralInRange<int>(0, 14));
      CheckInvariants();
      if (textEditor_.getText().size() > kMaxSourceSize) {
        LoadSource(provider_.PickValueInArray(kSeedSources));
      }
    }
  }

private:
  void LoadSource(std::string_view source) {
    if (!app_.loadFromString(source)) {
      return;
    }

    app_.setStructuredEditingEnabled(true);
    app_.document().document().setCanvasSize(180, 100);
    app_.setCleanSourceText(source);
    textEditor_.setText(source);
    textEditor_.resetTextChanged();
    previousSourceText_ = std::string(source);
    lastWritebackSourceText_.reset();
    selectionBoundsCache_ = SelectionBoundsCache();
    PumpEditor();
  }

  void RunAction(int action) {
    if (!app_.hasDocument()) {
      LoadSource(provider_.PickValueInArray(kSeedSources));
      return;
    }

    switch (action) {
      case 0: LoadSource(provider_.PickValueInArray(kSeedSources)); break;
      case 1: MoveSourceCursor(); break;
      case 2: InsertTextAtCursor(); break;
      case 3: ReplaceSourceRange(); break;
      case 4: DeleteSourceRange(); break;
      case 5: TextUndoOrRedo(); break;
      case 6: SelectNearSourceOffset(); break;
      case 7: SelectKnownId(); break;
      case 8: DeleteSelection(); break;
      case 9: UndoOrRedo(); break;
      case 10: SelectByHitTestOrRect(); break;
      case 11: ProbeFocusAtSourceOffset(); break;
      case 12: ProbeSelectionBounds(); break;
      case 13: RenderProbe(); break;
      case 14:
      default: PumpEditor(); break;
    }
  }

  void MoveSourceCursor() {
    const std::string text = textEditor_.getText();
    textEditor_.setCursorPosition(
        textEditor_.getCoordinatesAtByteOffset(ConsumeOffset(provider_, text)));
  }

  void InsertTextAtCursor() {
    MoveSourceCursor();
    textEditor_.insertText(ConsumeReplacement(provider_));
    PumpEditor();
  }

  void ReplaceSourceRange() {
    const std::string text = textEditor_.getText();
    const std::size_t start = ConsumeOffset(provider_, text);
    const std::size_t maxLength = std::min<std::size_t>(48, text.size() - start);
    const std::size_t length = provider_.ConsumeIntegralInRange<std::size_t>(0, maxLength);
    textEditor_.setSelection(textEditor_.getCoordinatesAtByteOffset(start),
                             textEditor_.getCoordinatesAtByteOffset(start + length));
    textEditor_.insertText(ConsumeReplacement(provider_));
    PumpEditor();
  }

  void DeleteSourceRange() {
    const std::string text = textEditor_.getText();
    const std::size_t start = ConsumeOffset(provider_, text);
    const std::size_t maxLength = std::min<std::size_t>(64, text.size() - start);
    const std::size_t length = provider_.ConsumeIntegralInRange<std::size_t>(0, maxLength);
    if (length == 0) {
      textEditor_.setCursorPosition(textEditor_.getCoordinatesAtByteOffset(start));
      if (provider_.ConsumeBool()) {
        textEditor_.backspace();
      } else {
        textEditor_.delete_();
      }
    } else {
      textEditor_.setSelection(textEditor_.getCoordinatesAtByteOffset(start),
                               textEditor_.getCoordinatesAtByteOffset(start + length));
      textEditor_.insertText("");
    }
    PumpEditor();
  }

  void TextUndoOrRedo() {
    if (provider_.ConsumeBool()) {
      textEditor_.undo();
    } else {
      textEditor_.redo();
    }
    PumpEditor();
  }

  void SelectNearSourceOffset() {
    const std::string source = CurrentDocumentSource();
    if (source.empty()) {
      return;
    }

    const std::size_t offset = ConsumeOffset(provider_, source);
    std::optional<svg::SVGElement> element =
        FindElementNearSourceOffset(app_.document().document(), source, offset);
    if (element.has_value()) {
      app_.setSelection(*element);
      textEditor_.setSelection(textEditor_.getCoordinatesAtByteOffset(offset),
                               textEditor_.getCoordinatesAtByteOffset(offset));
    } else {
      app_.clearSelection();
    }
  }

  void SelectKnownId() {
    const std::string_view id = provider_.PickValueInArray(kKnownIds);
    std::optional<svg::SVGElement> element =
        app_.document().document().querySelector("#" + std::string(id));
    if (element.has_value()) {
      app_.setSelection(*element);
    } else if (provider_.ConsumeBool()) {
      app_.clearSelection();
    }
  }

  void DeleteSelection() {
    if (!HasSynchronizedSource()) {
      return;
    }

    (void)app_.deleteSelectionWithUndo(textEditor_.getText());
    PumpEditor();
  }

  void UndoOrRedo() {
    if (provider_.ConsumeBool()) {
      app_.undo();
    } else {
      app_.redo();
    }
    PumpEditor();
  }

  void SelectByHitTestOrRect() {
    const Vector2d start = ConsumeDocumentPoint(provider_);
    if (!provider_.ConsumeBool()) {
      if (std::optional<svg::SVGGraphicsElement> hit = app_.hitTest(start)) {
        app_.setSelection(hit->cast<svg::SVGElement>());
      } else {
        app_.clearSelection();
      }
      return;
    }

    const Vector2d end = ConsumeDocumentPoint(provider_);
    const Box2d box = Box2d::FromXYWH(std::min(start.x, end.x), std::min(start.y, end.y),
                                      std::abs(end.x - start.x), std::abs(end.y - start.y));
    const std::vector<svg::SVGGraphicsElement> hits = app_.hitTestRect(box);
    std::vector<svg::SVGElement> elements;
    elements.reserve(hits.size());
    for (const svg::SVGGraphicsElement& hit : hits) {
      elements.push_back(hit.cast<svg::SVGElement>());
    }
    app_.setSelection(std::move(elements));
  }

  void ProbeFocusAtSourceOffset() {
    const std::string source = CurrentDocumentSource();
    if (source.empty()) {
      return;
    }

    const std::size_t offset = ConsumeOffset(provider_, source);
    if (std::optional<StyleFocus> focus =
            ComputeStyleFocusAtSourceOffset(app_.document().document(), offset)) {
      CheckElements(focus->impactedElements);
      CheckPartition(focus->partition);
    }

    if (!app_.selectedElements().empty()) {
      FocusPartition partition =
          ComputeFocusPartition(app_.document().document(), app_.selectedElements());
      CheckPartition(partition);
    }
  }

  void ProbeSelectionBounds() {
    RefreshSelectionBoundsCache(
        selectionBoundsCache_, std::span<const svg::SVGElement>(app_.selectedElements()),
        app_.document().currentFrameVersion(), app_.document().currentFrameVersion());
    PromoteSelectionBoundsIfReady(selectionBoundsCache_, app_.document().currentFrameVersion());
  }

  void RenderProbe() {
    svg::Renderer renderer;
    renderer.draw(app_.document().document());
    (void)renderer.takeSnapshot();
  }

  void PumpEditor() {
    if (!app_.hasDocument()) {
      return;
    }

    DispatchPendingSourceTextChange();
    for (int i = 0; i < 4; ++i) {
      (void)app_.flushFrame();
      MirrorPreservingReparseIntoTextEditor();
      DrainElementRemoveWritebacks();
      DispatchPendingSourceTextChange();
    }
  }

  void DispatchPendingSourceTextChange() {
    if (!app_.hasDocument() || !textEditor_.isTextChanged()) {
      return;
    }

    const std::string newSource = textEditor_.getText();
    std::vector<SourceEditIntent> intents = textEditor_.takePendingSourceEditIntents();
    app_.syncDirtyFromSource(newSource);
    textEditor_.resetTextChanged();
    if (intents.empty()) {
      (void)DispatchSourceTextChange(app_, newSource, &previousSourceText_,
                                     &lastWritebackSourceText_);
    } else {
      (void)DispatchSourceEditIntents(app_, intents, newSource, &previousSourceText_,
                                      &lastWritebackSourceText_);
    }
  }

  void MirrorPreservingReparseIntoTextEditor() {
    const auto& lastFlush = app_.document().lastFlushResult();
    if (!lastFlush.replacedDocument || !lastFlush.preserveUndoOnReparse || !app_.hasDocument() ||
        app_.document().lastParseError().has_value()) {
      return;
    }

    const std::string source = std::string(app_.document().document().source());
    if (source == textEditor_.getText()) {
      return;
    }

    textEditor_.setText(source, /*preserveScroll=*/true);
    textEditor_.resetTextChanged();
    previousSourceText_ = source;
    lastWritebackSourceText_ = source;
  }

  void DrainElementRemoveWritebacks() {
    std::vector<EditorApp::CompletedElementRemoveWriteback> completed =
        app_.consumeElementRemoveWritebacks();
    if (completed.empty()) {
      return;
    }

    std::string source = textEditor_.getText();
    std::vector<TextPatch> patches;
    patches.reserve(completed.size());
    for (const auto& writeback : completed) {
      if (std::optional<TextPatch> patch = buildElementRemoveWriteback(source, writeback.target);
          patch.has_value()) {
        patches.push_back(std::move(*patch));
      }
    }
    if (patches.empty()) {
      return;
    }

    const ApplyPatchesResult result = applyPatches(source, patches);
    if (result.applied != patches.size()) {
      return;
    }

    textEditor_.setText(source, /*preserveScroll=*/true);
    textEditor_.resetTextChanged();
    QueueSourceWritebackReparse(app_, source, &previousSourceText_, &lastWritebackSourceText_);
  }

  bool HasSynchronizedSource() const {
    return app_.hasDocument() && !textEditor_.isTextChanged() &&
           !app_.document().hasPendingMutations() &&
           textEditor_.getText() == app_.document().document().source();
  }

  std::string CurrentDocumentSource() const {
    if (!app_.hasDocument() || !app_.document().document().hasSourceStore()) {
      return {};
    }

    return std::string(app_.document().document().source());
  }

  void CheckInvariants() {
    if (!app_.hasDocument()) {
      return;
    }

    CheckElements(app_.selectedElements());
    if (HasSynchronizedSource()) {
      UTILS_RELEASE_ASSERT(textEditor_.getText() == app_.document().document().source());
    }
  }

  void CheckElements(std::span<const svg::SVGElement> elements) const {
    for (const svg::SVGElement& element : elements) {
      UTILS_RELEASE_ASSERT_MSG(IsElementInCurrentDocument(app_, element),
                               "editor state holds a stale SVG element");
    }
  }

  void CheckPartition(const FocusPartition& partition) const {
    for (const FocusReferenceLink& link : partition.referenceLinks) {
      UTILS_RELEASE_ASSERT(link.from.line >= 0);
      UTILS_RELEASE_ASSERT(link.from.column >= 0);
      UTILS_RELEASE_ASSERT(link.to.line >= 0);
      UTILS_RELEASE_ASSERT(link.to.column >= 0);
    }
  }

  FuzzedDataProvider& provider_;
  EditorApp app_;
  TextEditorCore textEditor_;
  std::string previousSourceText_;
  std::optional<std::string> lastWritebackSourceText_;
  SelectionBoundsCache selectionBoundsCache_;
};

}  // namespace

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > kMaxInputSize) {
    return 0;
  }

  FuzzedDataProvider provider(data, size);
  HeadlessEditorFuzzSession session(provider);
  session.Run();
  return 0;
}

}  // namespace donner::editor
