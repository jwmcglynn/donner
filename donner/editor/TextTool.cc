#include "donner/editor/TextTool.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "donner/base/FormatNumber.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorCommand.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
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

}  // namespace

void TextTool::onMouseDown(EditorApp& editor, const Vector2d& documentPoint,
                           MouseModifiers modifiers) {
  if (!editor.document().hasDocument()) {
    cancel();
    return;
  }

  dragToleranceDoc_ = kBoxDragScreenTolerance / std::max(modifiers.pixelsPerDocUnit, 0.000001);

  if (state_ == State::Editing) {
    // A click inside the session's text moves the caret; a click anywhere
    // else commits the session and starts a new draft at the click point.
    if (sessionText_.has_value()) {
      const long hitChar = sessionText_->withWriteAccess(
          [this, &documentPoint](svg::DocumentWriteAccess&, EntityHandle) {
            return sessionText_->getCharNumAtPosition(documentPoint);
          });
      if (hitChar >= 0) {
        // Map the DOM character index back to a logical caret index by
        // skipping over the hard breaks (which have no DOM character).
        std::size_t domIndex = 0;
        std::size_t logical = 0;
        for (; logical < content_.size(); ++logical) {
          if (content_[logical] == U'\n') {
            continue;
          }
          if (domIndex == static_cast<std::size_t>(hitChar)) {
            break;
          }
          ++domIndex;
        }
        caretIndex_ = logical;
        return;
      }
    }
    commit(editor);
    // Fall through to start a new draft from this click.
  }

  state_ = State::DraggingBox;
  dragStartDoc_ = documentPoint;
  dragBoxDoc_.reset();
}

void TextTool::onMouseMove(EditorApp& editor, const Vector2d& documentPoint, bool buttonHeld) {
  (void)editor;
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
  if (state_ != State::DraggingBox) {
    return;
  }

  const Vector2d dragDelta = documentPoint - dragStartDoc_;
  const bool isBox = std::hypot(dragDelta.x, dragDelta.y) > dragToleranceDoc_ &&
                     dragBoxDoc_.has_value() && dragBoxDoc_->size().x > dragToleranceDoc_;
  if (isBox) {
    // Box text: the origin is the box's top-left with the first baseline one
    // font-size below the top.
    const Box2d box = *dragBoxDoc_;
    beginEditingSession(editor, Vector2d(box.topLeft.x, box.topLeft.y + kDefaultFontSize), box);
  } else {
    // Point text: the click point is the first baseline origin.
    beginEditingSession(editor, dragStartDoc_, std::nullopt);
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
  text.setAttribute("fill", std::string(kDefaultFill));
  if (boxDoc.has_value()) {
    text.setAttribute("data-donner-text-box-width",
                      donner::detail::FormatNumberForSVG(boxDoc->size().x));
    text.setAttribute("data-donner-text-box-height",
                      donner::detail::FormatNumberForSVG(boxDoc->size().y));
  }

  previousSelection_ = editor.selectedElements();
  sessionText_ = text;
  boxDoc_ = boxDoc;
  originDoc_ = originDoc;
  fontSize_ = kDefaultFontSize;
  content_.clear();
  cachedCharWidths_.clear();
  caretIndex_ = 0;
  state_ = State::Editing;

  editor.applyMutation(EditorCommand::InsertTextCommand(parent, text, ""));
  editor.setSelection(text);
  editor.flushFrame();
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
  if (empty && sessionText_.has_value()) {
    // An empty session leaves the document unchanged: delete the created
    // element (soft delete, no undo entry) and restore the prior selection.
    editor.applyMutation(EditorCommand::DeleteElementCommand(*sessionText_));
    if (previousSelection_.empty()) {
      editor.clearSelection();
    } else {
      editor.setSelection(previousSelection_);
    }
    editor.flushFrame();
  } else if (sessionText_.has_value() && sessionBeforeSource_.has_value()) {
    editor.recordDocumentSourceUndoOnNextFlush("Insert text", *sessionText_, *sessionBeforeSource_);
  }

  cancel();
  return true;
}

void TextTool::cancel() {
  state_ = State::Idle;
  sessionText_.reset();
  boxDoc_.reset();
  dragBoxDoc_.reset();
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
        tspan.setAttribute("x", donner::detail::FormatNumberForSVG(originDoc_.x));
        if (i > 0) {
          tspan.setAttribute("dy", donner::detail::FormatNumberForSVG(lineHeight));
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
  if (boxDoc_.has_value()) {
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

  if (!boxDoc_.has_value() || cachedCharWidths_.empty()) {
    return hardLines;
  }

  // Greedy word wrap against the box width, using the measured width of each
  // non-newline code point. `widthIndex` walks cachedCharWidths_ in lockstep
  // with the non-newline code points of content_.
  const double boxWidth = boxDoc_->size().x;
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
  // caret on its display line. Caret Y: the line's baseline.
  double caretX = originDoc_.x;
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
  const double baselineY = originDoc_.y + static_cast<double>(line) * lineHeight;

  EditingChrome chrome;
  // Approximate ascent/descent from the font size; exact metrics are not
  // needed for a caret.
  chrome.caretTopDoc = Vector2d(caretX, baselineY - fontSize_ * 0.9);
  chrome.caretBottomDoc = Vector2d(caretX, baselineY + fontSize_ * 0.25);
  chrome.boxDoc = boxDoc_;
  return chrome;
}

}  // namespace donner::editor
