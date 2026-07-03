#include "donner/editor/TextTool.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "donner/base/FormatNumber.h"
#include "donner/base/parser/NumberParser.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/LockState.h"
#include "donner/editor/SelectionTransformHandles.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/SVGTSpanElement.h"
#include "donner/svg/SVGTextContentElement.h"

namespace donner::editor {

namespace {

/// Returns the element the new text should be inserted into: the selected
/// element when it is a compatible container (a `<g>` or the root `<svg>`),
/// otherwise the document's root `<svg>`.
svg::SVGElement ResolveInsertParent(EditorApp& editor) {
  const std::vector<svg::SVGElement>& selection = editor.selectedElements();
  if (selection.size() == 1u) {
    const svg::ElementType type = selection.front().type();
    if (type == svg::ElementType::G || type == svg::ElementType::SVG) {
      return selection.front();
    }
  }
  return editor.document().document().svgElement();
}

std::string Utf8FromCodepoints(const std::u32string& codepoints) {
  std::string out;
  out.reserve(codepoints.size());
  for (const char32_t cp : codepoints) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

/// True for code points the session accepts from the keyboard queue.
bool IsInsertableCodepoint(char32_t cp) {
  if (cp == U'\n' || cp == U'\r' || cp == U'\t') {
    return false;  // Enter/Tab route through their own handlers.
  }
  return cp >= 0x20 && cp != 0x7F;
}

std::u32string CodepointsFromUtf8(std::string_view utf8) {
  std::u32string out;
  out.reserve(utf8.size());
  std::size_t i = 0;
  while (i < utf8.size()) {
    const unsigned char lead = static_cast<unsigned char>(utf8[i]);
    char32_t cp = 0;
    std::size_t length = 1;
    if (lead < 0x80) {
      cp = lead;
    } else if ((lead & 0xE0) == 0xC0) {
      cp = lead & 0x1F;
      length = 2;
    } else if ((lead & 0xF0) == 0xE0) {
      cp = lead & 0x0F;
      length = 3;
    } else if ((lead & 0xF8) == 0xF0) {
      cp = lead & 0x07;
      length = 4;
    } else {
      ++i;  // Skip a stray continuation byte.
      continue;
    }
    if (i + length > utf8.size()) {
      break;
    }
    bool valid = true;
    for (std::size_t k = 1; k < length; ++k) {
      const unsigned char continuation = static_cast<unsigned char>(utf8[i + k]);
      if ((continuation & 0xC0) != 0x80) {
        valid = false;
        break;
      }
      cp = (cp << 6) | (continuation & 0x3F);
    }
    if (!valid) {
      ++i;
      continue;
    }
    out.push_back(cp);
    i += length;
  }
  return out;
}

/// Parse @p name as a plain SVG number, or nullopt when absent/malformed.
std::optional<double> ParseNumericAttribute(const svg::SVGElement& element, std::string_view name) {
  const std::optional<RcString> value = element.getAttribute(name);
  if (!value.has_value()) {
    return std::nullopt;
  }
  const auto result = ::donner::parser::NumberParser::Parse(std::string_view(*value));
  if (result.hasError()) {
    return std::nullopt;
  }
  return result.result().number;
}

/// Document-space AABB covering @p frameLocal mapped corner-by-corner
/// through @p documentFromText.
Box2d FrameDocAabb(const Transform2d& documentFromText, const Box2d& frameLocal) {
  const std::array<Vector2d, 4> corners = {
      frameLocal.topLeft, Vector2d(frameLocal.bottomRight.x, frameLocal.topLeft.y),
      frameLocal.bottomRight, Vector2d(frameLocal.topLeft.x, frameLocal.bottomRight.y)};
  Box2d frameDoc = Box2d::CreateEmpty(documentFromText.transformPosition(corners[0]));
  for (std::size_t i = 1; i < corners.size(); ++i) {
    frameDoc.addPoint(documentFromText.transformPosition(corners[i]));
  }
  return frameDoc;
}

/// Map a DOM character index (which counts rendered glyphs only) back to a
/// logical caret index into @p content (which also stores '\n' hard breaks).
std::size_t LogicalIndexForDomChar(const std::u32string& content, std::size_t domChar) {
  std::size_t domIndex = 0;
  std::size_t logical = 0;
  for (; logical < content.size(); ++logical) {
    if (content[logical] == U'\n') {
      continue;
    }
    if (domIndex == domChar) {
      break;
    }
    ++domIndex;
  }
  return logical;
}

}  // namespace

void TextTool::onMouseDown(EditorApp& editor, const Vector2d& documentPoint,
                           MouseModifiers modifiers) {
  if (!editor.document().hasDocument()) {
    cancel();
    return;
  }

  dragToleranceDoc_ = kBoxDragScreenTolerance / std::max(modifiers.pixelsPerDocUnit, 0.000001);

  if (state_ == State::Editing) {
    // A press on the frame's transform handles starts a frame gesture; a
    // click inside the session's text moves the caret (inside the frame but
    // off every glyph parks it at the end); a click anywhere else commits
    // the session before the idle click rules run below.
    if (sessionText_.has_value()) {
      if (beginFrameGestureAtPoint(documentPoint, modifiers)) {
        return;
      }
      if (const std::optional<std::size_t> caret = caretIndexAtPoint(documentPoint);
          caret.has_value()) {
        caretIndex_ = *caret;
        return;
      }
      if (const std::optional<Box2d> frameLocal = sessionFrameLocal(); frameLocal.has_value()) {
        const Vector2d localPoint = documentFromText_.inverse().transformPosition(documentPoint);
        if (frameLocal->contains(localPoint)) {
          caretIndex_ = content_.size();
          return;
        }
      }
    }
    commit(editor);
  }

  // A click on existing (unlocked) text opens an editing session on it with
  // the caret at the clicked character.
  if (const std::optional<svg::SVGGraphicsElement> hit = editor.hitTest(documentPoint);
      hit.has_value() && !IsLocked(*hit)) {
    const std::optional<svg::SVGTextElement> hitText = hit->withReadAccess(
        [&hit](svg::DocumentReadAccess&, EntityHandle) -> std::optional<svg::SVGTextElement> {
          return hit->isa<svg::SVGTextElement>()
                     ? std::make_optional(hit->cast<svg::SVGTextElement>())
                     : std::nullopt;
        });
    if (hitText.has_value()) {
      beginEditingSessionForExisting(editor, *hitText, documentPoint);
      return;
    }
  }

  // Empty canvas: start a (potential) box drag. Release decides between box
  // text (drag), point text (double-click), and nothing (plain click).
  state_ = State::DraggingBox;
  dragStartDoc_ = documentPoint;
  dragBoxDoc_.reset();
  pendingDoubleClick_ = modifiers.doubleClick;
}

void TextTool::onMouseMove(EditorApp& editor, const Vector2d& documentPoint, bool buttonHeld) {
  if (frameGesture_ != FrameGesture::None) {
    if (buttonHeld) {
      updateFrameGesture(editor, documentPoint);
    }
    return;
  }

  if (state_ != State::DraggingBox || !buttonHeld) {
    return;
  }

  const Vector2d topLeft(std::min(dragStartDoc_.x, documentPoint.x),
                         std::min(dragStartDoc_.y, documentPoint.y));
  const Vector2d bottomRight(std::max(dragStartDoc_.x, documentPoint.x),
                             std::max(dragStartDoc_.y, documentPoint.y));
  dragBoxDoc_ = Box2d(topLeft, bottomRight);
}

void TextTool::onMouseUp(EditorApp& editor, const Vector2d& documentPoint) {
  if (frameGesture_ != FrameGesture::None) {
    // Frame gestures end on release; the editing session stays open.
    frameGesture_ = FrameGesture::None;
    return;
  }

  if (state_ != State::DraggingBox) {
    return;
  }

  const Vector2d dragDelta = documentPoint - dragStartDoc_;
  const bool isBox = std::hypot(dragDelta.x, dragDelta.y) > dragToleranceDoc_ &&
                     dragBoxDoc_.has_value() && dragBoxDoc_->size().x > dragToleranceDoc_;
  const bool wantsPointText = pendingDoubleClick_;
  pendingDoubleClick_ = false;
  if (isBox) {
    // Box text: the origin is the box's top-left with the first baseline one
    // font-size below the top.
    const Box2d box = *dragBoxDoc_;
    beginEditingSession(editor, Vector2d(box.topLeft.x, box.topLeft.y + kDefaultFontSize), box);
  } else if (wantsPointText) {
    // Double-click on empty canvas: the click point is the first baseline
    // origin of new point text.
    beginEditingSession(editor, dragStartDoc_, std::nullopt);
  } else {
    // A plain click on empty canvas creates nothing (it already committed
    // any active session on mouse-down).
    state_ = State::Idle;
  }
  dragBoxDoc_.reset();
}

void TextTool::beginEditingSession(EditorApp& editor, const Vector2d& originDoc,
                                   const std::optional<Box2d>& boxDoc) {
  beginSessionUndo(editor);

  svg::SVGDocument& document = editor.document().document();
  svg::SVGElement parent = ResolveInsertParent(editor);

  svg::SVGTextElement text = svg::SVGTextElement::Create(document);
  text.setAttribute("x", donner::detail::FormatNumberForSVG(originDoc.x));
  text.setAttribute("y", donner::detail::FormatNumberForSVG(originDoc.y));
  text.setAttribute("font-family", std::string(kDefaultFontFamily));
  text.setAttribute("font-size", donner::detail::FormatNumberForSVG(kDefaultFontSize));
  // Fill goes through the style attribute — the same channel the editor's
  // fill-color picker edits — rather than a presentation attribute.
  text.setAttribute("style", "fill: " + std::string(kDefaultFill));
  if (boxDoc.has_value()) {
    text.setAttribute("data-donner-text-box-width",
                      donner::detail::FormatNumberForSVG(boxDoc->size().x));
    text.setAttribute("data-donner-text-box-height",
                      donner::detail::FormatNumberForSVG(boxDoc->size().y));
  }

  previousSelection_ = editor.selectedElements();
  sessionText_ = text;
  createdBySession_ = true;
  documentFromText_ = Transform2d();
  boxText_ = boxDoc;
  originText_ = originDoc;
  fontSize_ = kDefaultFontSize;
  content_.clear();
  cachedCharWidths_.clear();
  caretIndex_ = 0;
  state_ = State::Editing;

  editor.applyMutation(EditorCommand::InsertTextCommand(parent, text, ""));
  editor.setSelection(text);
  editor.flushFrame();
}

void TextTool::beginEditingSessionForExisting(EditorApp& editor, const svg::SVGTextElement& text,
                                              const Vector2d& documentPoint) {
  beginSessionUndo(editor);

  previousSelection_ = editor.selectedElements();
  sessionText_ = text;
  createdBySession_ = false;

  // Every DOM read below (children, text content, attributes, transforms,
  // character geometry) requires a scoped access; one write scope covers the
  // whole reconstruction.
  text.withWriteAccess([this, &text](svg::DocumentWriteAccess&, EntityHandle) {
    documentFromText_ = text.elementFromWorld();

    // Reconstruct the logical content from the DOM. Tool-authored text is
    // either a bare text node or one <tspan> per display line, where
    // soft-wrapped continuation lines carry `data-donner-soft-wrap` (joined
    // back without a break — the wrap is recomputed). Foreign tspans without
    // the marker reconstruct as hard line breaks.
    content_.clear();
    bool sawTspan = false;
    for (std::optional<svg::SVGElement> child = text.firstChild(); child.has_value();
         child = child->nextSibling()) {
      if (child->type() != svg::ElementType::TSpan) {
        continue;
      }
      const bool softWrap = child->getAttribute("data-donner-soft-wrap").has_value();
      if (sawTspan && !softWrap) {
        content_.push_back(U'\n');
      }
      content_ += CodepointsFromUtf8(child->cast<svg::SVGTSpanElement>().textContent());
      sawTspan = true;
    }
    if (!sawTspan) {
      content_ = CodepointsFromUtf8(text.textContent());
    }

    fontSize_ = ParseNumericAttribute(text, "font-size").value_or(kDefaultFontSize);

    const std::optional<double> xAttr = ParseNumericAttribute(text, "x");
    const std::optional<double> yAttr = ParseNumericAttribute(text, "y");
    originText_ = Vector2d(xAttr.value_or(0.0), yAttr.value_or(0.0));
    if ((!xAttr.has_value() || !yAttr.has_value()) && text.getNumberOfChars() > 0) {
      // Fall back to the first glyph's pen position for foreign text that
      // positions itself through tspans instead of root attributes.
      const Vector2d firstPen = text.getStartPositionOfChar(0);
      originText_ = Vector2d(xAttr.value_or(firstPen.x), yAttr.value_or(firstPen.y));
    }

    boxText_.reset();
    const std::optional<double> boxWidth =
        ParseNumericAttribute(text, "data-donner-text-box-width");
    const std::optional<double> boxHeight =
        ParseNumericAttribute(text, "data-donner-text-box-height");
    if (boxWidth.has_value() && boxHeight.has_value()) {
      // Invert the creation rule: the origin sits one font-size below the
      // box's top-left corner.
      const Vector2d topLeft(originText_.x, originText_.y - fontSize_);
      boxText_ = Box2d(topLeft, topLeft + Vector2d(*boxWidth, *boxHeight));
    }
  });

  dragBoxDoc_.reset();
  state_ = State::Editing;
  cachedCharWidths_ = measureCharacterWidths(editor);
  caretIndex_ = caretIndexAtPoint(documentPoint).value_or(content_.size());

  editor.setSelection(text);
  editor.flushFrame();
}

std::optional<std::size_t> TextTool::caretIndexAtPoint(const Vector2d& documentPoint) const {
  if (!sessionText_.has_value()) {
    return std::nullopt;
  }

  const Vector2d textPoint = documentFromText_.inverse().transformPosition(documentPoint);
  struct CharHit {
    long index = -1;
    bool afterCenter = false;
  };
  const CharHit hit = sessionText_->withWriteAccess([this, &textPoint](svg::DocumentWriteAccess&,
                                                                       EntityHandle) -> CharHit {
    CharHit result;
    result.index = sessionText_->getCharNumAtPosition(textPoint);
    if (result.index >= 0) {
      const Box2d extent = sessionText_->getExtentOfChar(static_cast<std::size_t>(result.index));
      result.afterCenter = textPoint.x > (extent.topLeft.x + extent.bottomRight.x) * 0.5;
    }
    return result;
  });
  if (hit.index < 0) {
    return std::nullopt;
  }

  const std::size_t logical = LogicalIndexForDomChar(content_, static_cast<std::size_t>(hit.index));
  // A click in the trailing half of a glyph places the caret after it.
  return hit.afterCenter ? std::min(logical + 1u, content_.size()) : logical;
}

std::optional<Box2d> TextTool::sessionFrameLocal() const {
  if (!sessionText_.has_value()) {
    return std::nullopt;
  }
  if (boxText_.has_value()) {
    return boxText_;
  }

  // Point text: the frame is the computed ink extent.
  const Box2d inkLocal = sessionText_->withWriteAccess(
      [this](svg::DocumentWriteAccess&, EntityHandle) { return sessionText_->inkBoundingBox(); });
  if (inkLocal.isEmpty()) {
    return std::nullopt;
  }
  return inkLocal;
}

bool TextTool::beginFrameGestureAtPoint(const Vector2d& documentPoint, MouseModifiers modifiers) {
  const std::optional<Box2d> frameLocal = sessionFrameLocal();
  if (!frameLocal.has_value()) {
    return false;
  }

  const Box2d frameDoc = FrameDocAabb(documentFromText_, *frameLocal);
  const std::array<Box2d, 1> bounds{frameDoc};
  const SelectionTransformHandleIntent intent = HitTestSelectionTransformHandles(
      std::span<const Box2d>(bounds), documentPoint, modifiers.pixelsPerDocUnit,
      /*includeRotate=*/!modifiers.shift);
  if (intent.kind == SelectionTransformHandleKind::None) {
    return false;
  }

  frameStartLocal_ = *frameLocal;
  frameStartDocumentFromText_ = documentFromText_;
  frameCorner_ = intent.corner;
  if (intent.kind == SelectionTransformHandleKind::Resize) {
    frameGesture_ = FrameGesture::Resize;
  } else {
    frameGesture_ = FrameGesture::Rotate;
    rotateCenterDoc_ = (frameDoc.topLeft + frameDoc.bottomRight) * 0.5;
    rotateStartAngleRadians_ = AngleFromCenter(rotateCenterDoc_, documentPoint);
    rotateStartTransform_ =
        sessionText_->withReadAccess([this](svg::DocumentReadAccess&, EntityHandle) {
          return sessionText_->cast<svg::SVGGraphicsElement>().transform();
        });
  }
  return true;
}

void TextTool::updateFrameGesture(EditorApp& editor, const Vector2d& documentPoint) {
  if (frameGesture_ == FrameGesture::None || !sessionText_.has_value()) {
    return;
  }

  if (frameGesture_ == FrameGesture::Rotate) {
    const double angleDelta =
        AngleFromCenter(rotateCenterDoc_, documentPoint) - rotateStartAngleRadians_;
    const Transform2d documentFromStartDocument =
        TransformDocumentAroundPoint(rotateCenterDoc_, Transform2d::Rotate(angleDelta));
    editor.applyMutation(EditorCommand::SetTransformCommand(
        *sessionText_, rotateStartTransform_ * documentFromStartDocument));
    editor.flushFrame();
    // The element transform changed under the session — refresh the local→
    // document mapping so caret math and subsequent frame hit-tests track
    // the rotated frame.
    documentFromText_ =
        sessionText_->withWriteAccess([this](svg::DocumentWriteAccess&, EntityHandle) {
          return sessionText_->elementFromWorld();
        });
    return;
  }

  // Frame resize: recompute the frame in the text's local space with the
  // corner opposite the grab anchored. The glyphs never scale — the box
  // attributes change and the content rewraps to the new width. A resize of
  // point text writes box attributes for the first time, converting the
  // computed frame into a user-sized one.
  const Vector2d localPoint =
      frameStartDocumentFromText_.inverse().transformPosition(documentPoint);
  const Vector2d anchor = SelectionTransformCornerPoint(
      frameStartLocal_, OppositeSelectionTransformCorner(frameCorner_));
  const Vector2d startCorner = SelectionTransformCornerPoint(frameStartLocal_, frameCorner_);

  // Clamp the moving corner to its side of the anchor so the frame never
  // inverts or collapses.
  constexpr double kMinFrameExtent = 1.0;
  const double signX = startCorner.x >= anchor.x ? 1.0 : -1.0;
  const double signY = startCorner.y >= anchor.y ? 1.0 : -1.0;
  const double extentX = std::max(kMinFrameExtent, (localPoint.x - anchor.x) * signX);
  const double extentY = std::max(kMinFrameExtent, (localPoint.y - anchor.y) * signY);
  const Vector2d movingCorner = anchor + Vector2d(extentX * signX, extentY * signY);
  const Box2d newFrame(
      Vector2d(std::min(anchor.x, movingCorner.x), std::min(anchor.y, movingCorner.y)),
      Vector2d(std::max(anchor.x, movingCorner.x), std::max(anchor.y, movingCorner.y)));

  boxText_ = newFrame;
  originText_ = Vector2d(newFrame.topLeft.x, newFrame.topLeft.y + fontSize_);
  editor.applyMutation(EditorCommand::SetAttributeCommand(
      *sessionText_, "x", donner::detail::FormatNumberForSVG(originText_.x)));
  editor.applyMutation(EditorCommand::SetAttributeCommand(
      *sessionText_, "y", donner::detail::FormatNumberForSVG(originText_.y)));
  editor.applyMutation(
      EditorCommand::SetAttributeCommand(*sessionText_, "data-donner-text-box-width",
                                         donner::detail::FormatNumberForSVG(newFrame.size().x)));
  editor.applyMutation(
      EditorCommand::SetAttributeCommand(*sessionText_, "data-donner-text-box-height",
                                         donner::detail::FormatNumberForSVG(newFrame.size().y)));
  syncContentToDom(editor);
}

void TextTool::insertCodepoint(EditorApp& editor, char32_t codepoint) {
  if (state_ != State::Editing || !IsInsertableCodepoint(codepoint)) {
    return;
  }
  content_.insert(content_.begin() + static_cast<std::ptrdiff_t>(caretIndex_), codepoint);
  ++caretIndex_;
  syncContentToDom(editor);
}

void TextTool::insertNewline(EditorApp& editor) {
  if (state_ != State::Editing) {
    return;
  }
  content_.insert(content_.begin() + static_cast<std::ptrdiff_t>(caretIndex_), U'\n');
  ++caretIndex_;
  syncContentToDom(editor);
}

void TextTool::backspace(EditorApp& editor) {
  if (state_ != State::Editing || caretIndex_ == 0) {
    return;
  }
  content_.erase(content_.begin() + static_cast<std::ptrdiff_t>(caretIndex_) - 1);
  --caretIndex_;
  syncContentToDom(editor);
}

void TextTool::deleteForward(EditorApp& editor) {
  if (state_ != State::Editing || caretIndex_ >= content_.size()) {
    return;
  }
  content_.erase(content_.begin() + static_cast<std::ptrdiff_t>(caretIndex_));
  syncContentToDom(editor);
}

void TextTool::moveCaret(EditorApp& editor, CaretMove move) {
  if (state_ != State::Editing) {
    return;
  }

  const std::vector<std::u32string> lines = displayLines(editor);
  const auto [line, column] = caretLineColumn(lines);

  switch (move) {
    case CaretMove::Left:
      if (caretIndex_ > 0) {
        --caretIndex_;
      }
      break;
    case CaretMove::Right:
      if (caretIndex_ < content_.size()) {
        ++caretIndex_;
      }
      break;
    case CaretMove::LineStart: caretIndex_ -= column; break;
    case CaretMove::LineEnd: {
      const std::u32string& current = lines[line];
      const std::size_t visibleLength =
          !current.empty() && current.back() == U'\n' ? current.size() - 1u : current.size();
      caretIndex_ += visibleLength - column;
      break;
    }
    case CaretMove::Up:
    case CaretMove::Down: {
      const std::size_t targetLine = move == CaretMove::Up ? (line > 0 ? line - 1u : 0u)
                                                           : std::min(line + 1u, lines.size() - 1u);
      if (targetLine == line) {
        break;
      }
      std::size_t index = 0;
      for (std::size_t i = 0; i < targetLine; ++i) {
        index += lines[i].size();
      }
      const std::u32string& target = lines[targetLine];
      const std::size_t targetVisible =
          !target.empty() && target.back() == U'\n' ? target.size() - 1u : target.size();
      caretIndex_ = index + std::min(column, targetVisible);
      break;
    }
  }
}

void TextTool::toggleBold(EditorApp& editor) {
  if (state_ != State::Editing || !sessionText_.has_value()) {
    return;
  }
  const bool bold = sessionText_->withReadAccess([this](svg::DocumentReadAccess&, EntityHandle) {
    const auto value = sessionText_->getAttribute("font-weight");
    return value.has_value() && *value == "bold";
  });
  editor.applyMutation(
      bold ? EditorCommand::RemoveAttributeCommand(*sessionText_, "font-weight")
           : EditorCommand::SetAttributeCommand(*sessionText_, "font-weight", "bold"));
  editor.flushFrame();
}

void TextTool::toggleItalic(EditorApp& editor) {
  if (state_ != State::Editing || !sessionText_.has_value()) {
    return;
  }
  const bool italic = sessionText_->withReadAccess([this](svg::DocumentReadAccess&, EntityHandle) {
    const auto value = sessionText_->getAttribute("font-style");
    return value.has_value() && *value == "italic";
  });
  editor.applyMutation(
      italic ? EditorCommand::RemoveAttributeCommand(*sessionText_, "font-style")
             : EditorCommand::SetAttributeCommand(*sessionText_, "font-style", "italic"));
  editor.flushFrame();
}

void TextTool::toggleUnderline(EditorApp& editor) {
  if (state_ != State::Editing || !sessionText_.has_value()) {
    return;
  }
  const bool underline =
      sessionText_->withReadAccess([this](svg::DocumentReadAccess&, EntityHandle) {
        const auto value = sessionText_->getAttribute("text-decoration");
        return value.has_value() && *value == "underline";
      });
  editor.applyMutation(
      underline
          ? EditorCommand::RemoveAttributeCommand(*sessionText_, "text-decoration")
          : EditorCommand::SetAttributeCommand(*sessionText_, "text-decoration", "underline"));
  editor.flushFrame();
}

bool TextTool::commit(EditorApp& editor) {
  if (state_ != State::Editing) {
    if (state_ == State::DraggingBox) {
      state_ = State::Idle;
      dragBoxDoc_.reset();
    }
    return false;
  }

  const bool empty = content_.empty();
  if (empty && sessionText_.has_value() && createdBySession_) {
    // An empty session leaves the document unchanged: delete the created
    // element (soft delete, no undo entry) and restore the prior selection.
    editor.applyMutation(EditorCommand::DeleteElementCommand(*sessionText_));
    if (previousSelection_.empty()) {
      editor.clearSelection();
    } else {
      editor.setSelection(previousSelection_);
    }
    editor.flushFrame();
  } else if (empty && sessionText_.has_value()) {
    // Emptying an existing element deletes it — as a real undoable edit,
    // since the pre-session document had content here.
    editor.applyMutation(EditorCommand::DeleteElementCommand(*sessionText_));
    editor.clearSelection();
    if (sessionBeforeSource_.has_value()) {
      editor.recordDocumentSourceUndoOnNextFlush(
          "Delete text", editor.document().document().svgElement(), *sessionBeforeSource_);
    }
    editor.flushFrame();
  } else if (sessionText_.has_value() && sessionBeforeSource_.has_value() &&
             editor.document().document().source() != *sessionBeforeSource_) {
    // Skip the undo entry when the session changed nothing (e.g. the user
    // clicked into existing text and clicked away without typing).
    editor.recordDocumentSourceUndoOnNextFlush(createdBySession_ ? "Insert text" : "Edit text",
                                               *sessionText_, *sessionBeforeSource_);
  }

  cancel();
  return true;
}

void TextTool::cancel() {
  state_ = State::Idle;
  frameGesture_ = FrameGesture::None;
  sessionText_.reset();
  createdBySession_ = true;
  documentFromText_ = Transform2d();
  boxText_.reset();
  dragBoxDoc_.reset();
  pendingDoubleClick_ = false;
  content_.clear();
  cachedCharWidths_.clear();
  caretIndex_ = 0;
  sessionBeforeSource_.reset();
  previousSelection_.clear();
}

void TextTool::syncContentToDom(EditorApp& editor) {
  if (!sessionText_.has_value()) {
    return;
  }

  const auto rebuild = [&](const std::vector<std::u32string>& lines) {
    // Remove existing <tspan> children; SetTextContent handles bare text.
    std::vector<svg::SVGElement> children;
    sessionText_->withReadAccess([this, &children](svg::DocumentReadAccess&, EntityHandle) {
      for (std::optional<svg::SVGElement> child = sessionText_->firstChild(); child.has_value();
           child = child->nextSibling()) {
        children.push_back(*child);
      }
    });
    for (const svg::SVGElement& child : children) {
      editor.applyMutation(EditorCommand::DeleteElementCommand(child));
    }

    const auto stripHardBreak = [](const std::u32string& line) {
      return !line.empty() && line.back() == U'\n' ? line.substr(0, line.size() - 1u) : line;
    };

    if (lines.size() <= 1u) {
      const std::u32string visible = lines.empty() ? std::u32string() : stripHardBreak(lines[0]);
      editor.applyMutation(
          EditorCommand::SetTextContentCommand(*sessionText_, Utf8FromCodepoints(visible)));
    } else {
      editor.applyMutation(EditorCommand::SetTextContentCommand(*sessionText_, ""));
      svg::SVGDocument& document = editor.document().document();
      const double lineHeight = fontSize_ * kLineHeightFactor;
      for (std::size_t i = 0; i < lines.size(); ++i) {
        svg::SVGTSpanElement tspan = svg::SVGTSpanElement::Create(document);
        tspan.setAttribute("x", donner::detail::FormatNumberForSVG(originText_.x));
        if (i > 0) {
          tspan.setAttribute("dy", donner::detail::FormatNumberForSVG(lineHeight));
          // A continuation line that did NOT follow a hard break is a soft
          // wrap; the marker lets a later editing session join it back
          // without a '\n' and recompute the wrap.
          const std::u32string& previous = lines[i - 1u];
          if (previous.empty() || previous.back() != U'\n') {
            tspan.setAttribute("data-donner-soft-wrap", "true");
          }
        }
        editor.applyMutation(EditorCommand::InsertTextCommand(
            *sessionText_, tspan, Utf8FromCodepoints(stripHardBreak(lines[i]))));
      }
    }
    editor.flushFrame();
  };

  // First pass with the current cached widths; then re-measure (used by both
  // caret math and wrapping) and re-wrap once if the new content changed the
  // break points (box text only).
  const std::vector<std::u32string> lines = displayLines(editor);
  rebuild(lines);
  cachedCharWidths_ = measureCharacterWidths(editor);
  if (boxText_.has_value()) {
    const std::vector<std::u32string> rewrapped = displayLines(editor);
    if (rewrapped != lines) {
      rebuild(rewrapped);
      cachedCharWidths_ = measureCharacterWidths(editor);
    }
  }
}

std::vector<std::u32string> TextTool::displayLines(EditorApp& editor) const {
  (void)editor;
  // Split on hard breaks; each stored line KEEPS its trailing '\n' so caret
  // index math can sum line sizes directly.
  std::vector<std::u32string> hardLines;
  std::u32string current;
  for (const char32_t cp : content_) {
    current.push_back(cp);
    if (cp == U'\n') {
      hardLines.push_back(std::move(current));
      current.clear();
    }
  }
  hardLines.push_back(std::move(current));

  if (!boxText_.has_value() || cachedCharWidths_.empty()) {
    return hardLines;
  }

  // Greedy word wrap against the box width, using the measured width of each
  // non-newline code point. `widthIndex` walks cachedCharWidths_ in lockstep
  // with the non-newline code points of content_.
  const double boxWidth = boxText_->size().x;
  std::vector<std::u32string> wrapped;
  std::size_t widthIndex = 0;
  for (const std::u32string& hardLine : hardLines) {
    std::u32string line;
    double lineWidth = 0.0;
    std::size_t lastSpaceInLine = std::u32string::npos;
    for (const char32_t cp : hardLine) {
      if (cp == U'\n') {
        line.push_back(cp);
        break;
      }
      const double width =
          widthIndex < cachedCharWidths_.size() ? cachedCharWidths_[widthIndex] : 0.0;
      ++widthIndex;
      line.push_back(cp);
      lineWidth += width;
      if (cp == U' ') {
        lastSpaceInLine = line.size() - 1u;
      }
      if (lineWidth > boxWidth && line.size() > 1u) {
        // Break after the last space when there is one; otherwise break
        // before this code point.
        std::u32string remainder;
        if (lastSpaceInLine != std::u32string::npos && lastSpaceInLine + 1u < line.size()) {
          remainder = line.substr(lastSpaceInLine + 1u);
          line.erase(lastSpaceInLine + 1u);
        } else {
          remainder.push_back(line.back());
          line.pop_back();
        }
        wrapped.push_back(std::move(line));
        line = std::move(remainder);
        lineWidth = 0.0;
        lastSpaceInLine = std::u32string::npos;
        for (std::size_t i = 0; i < line.size(); ++i) {
          const std::size_t remainderWidthIndex = widthIndex - line.size() + i;
          lineWidth += remainderWidthIndex < cachedCharWidths_.size()
                           ? cachedCharWidths_[remainderWidthIndex]
                           : 0.0;
          if (line[i] == U' ') {
            lastSpaceInLine = i;
          }
        }
      }
    }
    wrapped.push_back(std::move(line));
  }
  return wrapped;
}

std::vector<double> TextTool::measureCharacterWidths(EditorApp& editor) const {
  (void)editor;
  if (!sessionText_.has_value()) {
    return {};
  }

  return sessionText_->withWriteAccess(
      [this](svg::DocumentWriteAccess&, EntityHandle) -> std::vector<double> {
        std::vector<double> widths;
        const long count = sessionText_->getNumberOfChars();
        widths.reserve(static_cast<std::size_t>(std::max(0L, count)));
        for (long i = 0; i < count; ++i) {
          const Vector2d start = sessionText_->getStartPositionOfChar(static_cast<std::size_t>(i));
          const Vector2d end = sessionText_->getEndPositionOfChar(static_cast<std::size_t>(i));
          widths.push_back(std::abs(end.x - start.x));
        }
        return widths;
      });
}

std::pair<std::size_t, std::size_t> TextTool::caretLineColumn(
    const std::vector<std::u32string>& lines) const {
  std::size_t remaining = caretIndex_;
  for (std::size_t line = 0; line < lines.size(); ++line) {
    const std::u32string& stored = lines[line];
    const bool hardBreak = !stored.empty() && stored.back() == U'\n';
    const std::size_t visible = hardBreak ? stored.size() - 1u : stored.size();
    // A caret exactly at a hard break belongs to the END of this line; a
    // caret at a soft (wrap) boundary belongs to the START of the next line,
    // where the next typed character will land.
    if (remaining < visible || (hardBreak && remaining == visible) || line + 1u == lines.size()) {
      return {line, std::min(remaining, visible)};
    }
    remaining -= stored.size();
    if (!hardBreak && remaining == 0 && line + 1u < lines.size()) {
      return {line + 1u, 0u};
    }
  }
  return {lines.empty() ? 0u : lines.size() - 1u, 0u};
}

void TextTool::beginSessionUndo(EditorApp& editor) {
  if (sessionBeforeSource_.has_value()) {
    return;
  }
  if (editor.document().hasDocument() && editor.document().document().hasSourceStore()) {
    sessionBeforeSource_ = std::string(editor.document().document().source());
  }
}

std::optional<TextTool::EditingChrome> TextTool::editingChrome(EditorApp& editor) const {
  if (state_ != State::Editing || !sessionText_.has_value()) {
    return std::nullopt;
  }

  const std::vector<std::u32string> lines = displayLines(const_cast<EditorApp&>(editor));
  const auto [line, column] = caretLineColumn(lines);

  // Caret X: origin plus the measured widths of the code points before the
  // caret on its display line. Caret Y: the line's baseline. Both computed
  // in the text's local space, then mapped to document space.
  double caretX = originText_.x;
  std::size_t widthIndex = 0;
  for (std::size_t i = 0; i < line; ++i) {
    for (const char32_t cp : lines[i]) {
      if (cp != U'\n') {
        ++widthIndex;
      }
    }
  }
  for (std::size_t i = 0; i < column; ++i) {
    caretX += widthIndex < cachedCharWidths_.size() ? cachedCharWidths_[widthIndex] : 0.0;
    ++widthIndex;
  }

  const double lineHeight = fontSize_ * kLineHeightFactor;
  const double baselineY = originText_.y + static_cast<double>(line) * lineHeight;

  EditingChrome chrome;
  // Approximate ascent/descent from the font size; exact metrics are not
  // needed for a caret.
  chrome.caretTopDoc =
      documentFromText_.transformPosition(Vector2d(caretX, baselineY - fontSize_ * 0.9));
  chrome.caretBottomDoc =
      documentFromText_.transformPosition(Vector2d(caretX, baselineY + fontSize_ * 0.25));
  chrome.boxDoc = boxText_.has_value()
                      ? std::make_optional(documentFromText_.transformBox(*boxText_))
                      : std::nullopt;
  return chrome;
}

}  // namespace donner::editor
