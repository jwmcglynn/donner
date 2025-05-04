#pragma once
/// @file

#include "donner/base/Length.h"
#include "donner/base/RcString.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/core/LengthAdjust.h"

namespace donner::svg {

/**
 * Base class for elements that support rendering child text content.
 *
 * This class matches the behavior of the IDL interface `SVGTextContentElement`.
 * It inherits from \ref SVGGraphicsElement, but is not directly instantiable.
 *
 * \see https://www.w3.org/TR/SVG2/text.html#InterfaceSVGTextContentElement
 */
class SVGTextContentElement : public SVGGraphicsElement {
  friend class parser::SVGParserImpl;

protected:
  /**
   * Inheriting constructor to be called by derived classes. \ref SVGTextContentElement cannot
   * be instantiated directly.
   *
   * @param handle The handle to the underlying entity.
   */
  explicit SVGTextContentElement(EntityHandle handle);

public:
  /// Returns true if the given element type can be cast to \ref SVGTextContentElement.
  static constexpr bool IsBaseOf(ElementType type) {
    return type == ElementType::Text || type == ElementType::TSpan;
  }

  /**
   * Returns the textLength attribute (the author's intended length for the text).
   *
   * \see https://www.w3.org/TR/SVG2/text.html#TextElementTextLengthAttribute
   */
  std::optional<Lengthd> textLength() const;

  /**
   * Sets the textLength attribute, which indicates the author-computed total length for the text
   * layout.
   *
   * @param value Length value.
   */
  void setTextLength(std::optional<Lengthd> value);

  /**
   * Returns the lengthAdjust attribute, which controls how the text is stretched or spaced to fit
   * the textLength.
   *
   * \see https://www.w3.org/TR/SVG2/text.html#TextElementLengthAdjustAttribute
   */
  LengthAdjust lengthAdjust() const;

  /**
   * Controls how the text is stretched or spaced to fit the \ref textLength.
   *
   * @param value Adjustment mode, defaults to \ref LengthAdjust::Spacing.
   */
  void setLengthAdjust(LengthAdjust value);

  /**
   * Returns the total number of addressable characters in the element.
   * \see https://www.w3.org/TR/SVG2/text.html#__svg__SVGTextContentElement__getNumberOfChars
   */
  long getNumberOfChars() const;

  /**
   * Computes the total advance distance for all glyphs.
   * \see https://www.w3.org/TR/SVG2/text.html#__svg__SVGTextContentElement__getComputedTextLength
   */
  double getComputedTextLength() const;

  /**
   * Computes the advance distance for a substring of text.
   * The substring is defined by the character positions [charnum, charnum + nchars).
   * \see https://www.w3.org/TR/SVG2/text.html#__svg__SVGTextContentElement__getSubStringLength
   */
  double getSubStringLength(std::size_t charnum, std::size_t nchars) const;

  /**
   * Returns the start position (in user space) of the glyphs
   * corresponding to the given character index.
   * \see https://www.w3.org/TR/SVG2/text.html#__svg__SVGTextContentElement__getStartPositionOfChar
   */
  Vector2d getStartPositionOfChar(std::size_t charnum) const;

  /**
   * Returns the end position (in user space) of the glyphs
   * corresponding to the given character index.
   * \see https://www.w3.org/TR/SVG2/text.html#__svg__SVGTextContentElement__getEndPositionOfChar
   */
  Vector2d getEndPositionOfChar(std::size_t charnum) const;

  /**
   * Returns the bounding box of the glyph cell for the specified character,
   * in the element's coordinate space.
   * \see https://www.w3.org/TR/SVG2/text.html#__svg__SVGTextContentElement__getExtentOfChar
   */
  Boxd getExtentOfChar(std::size_t charnum) const;

  /**
   * Returns the rotation applied to the glyphs corresponding to the
   * given character index, in degrees.
   * \see https://www.w3.org/TR/SVG2/text.html#__svg__SVGTextContentElement__getRotationOfChar
   */
  double getRotationOfChar(std::size_t charnum) const;

  /**
   * Given a point in the element's coordinate space, returns which character
   * is rendered at that point. Returns -1 if none.
   * \see https://www.w3.org/TR/SVG2/text.html#__svg__SVGTextContentElement__getCharNumAtPosition
   */
  long getCharNumAtPosition(const Vector2d& point) const;

  /**
   * Select a substring of characters for user operations (e.g. text highlight).
   * \see https://www.w3.org/TR/SVG2/text.html#__svg__SVGTextContentElement__selectSubString
   */
  void selectSubString(std::size_t charnum, std::size_t nchars);

  /**
   * Append text content from text or CDATA nodes.
   *
   * @param text Text content to append.
   */
  void appendText(std::string_view text);

  /**
   * Get the raw text content concatenated from all child text nodes.
   *
   * @return Text content string.
   */
  RcString textContent() const;
};

}  // namespace donner::svg
