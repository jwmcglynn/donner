#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <ostream>
#include <span>
#include <vector>

#include "donner/base/Utils.h"
#include "donner/css/Declaration.h"
#include "donner/css/FontFace.h"
#include "donner/css/Selector.h"

namespace donner::css {

/// One layer segment in the sheet's local layer tree.
using LayerId = std::uint16_t;
/// The implicit sheet root and layer identity of unlayered rules.
inline constexpr LayerId kUnlayered = std::numeric_limits<LayerId>::max();

/// A layer name segment, scoped to its parent in the same sheet.
struct LayerNode {
  LayerId parent;  //!< Sheet-local parent, or the implicit root.
  RcString name;   //!< Exact decoded identifier. Empty denotes a new anonymous layer.
};

/// Source-order layer declarations and retained import positions.
struct LayerEvent {
  /// An explicit statement, a block, or a retained import.
  enum class Kind { Statement, Block, Import };
  Kind kind;
  std::uint32_t index;  //!< LayerId for a declaration, or index in Stylesheet::imports().
};

/// Immutable sheet-local layer syntax, allocated only for a sheet that uses layers.
struct StylesheetLayers {
  std::vector<LayerNode> nodes;
  std::vector<LayerEvent> events;
};

/**
 * A rule in a stylesheet, which consists of a selector and a list of declarations.
 *
 * For example, the following is a valid rule:
 * ```
 * path.withColor {
 *   fill: red;
 *   stroke: blue;
 * }
 * ```
 *
 * The selector is `path.withColor`, and the declarations are `fill: red` and `stroke: blue`.
 */
struct SelectorRule {
  Selector selector;                      ///< Selector for this rule.
  std::vector<Declaration> declarations;  ///< Declarations for this rule.
  SourceRange ruleSourceRange{FileOffset::Offset(0),
                              FileOffset::Offset(0)};  ///< Rule range in local CSS source.
  SourceRange selectorSourceRange{
      FileOffset::Offset(0), FileOffset::Offset(0)};  ///< Full selector range in local CSS source.
  std::vector<SourceRange>
      selectorEntrySourceRanges;  ///< Per-entry selector ranges in local CSS source.
  LayerId layer = kUnlayered;     //!< Sheet-local cascade layer.

  /**
   * Output a human-readable representation of the delector to a stream.
   *
   * @param os Output stream.
   * @param rule SelectorRule to output.
   */
  friend std::ostream& operator<<(std::ostream& os, const SelectorRule& rule) {
    os << rule.selector << " {\n";
    for (const auto& declaration : rule.declarations) {
      os << declaration << "\n";
    }
    return os << "}\n";
  }
};

/**
 * An unconditional `@import` rule retained by the stylesheet parser, such as
 * `@import url("theme.css");`.
 *
 * Only imports that apply unconditionally are retained. Imports with
 * `supports()` or media conditions other than `all`, and imports that appear after another rule,
 * are diagnosed and dropped by the parser. The parser never resolves or fetches the URL; loading is
 * the responsibility of the document that owns the stylesheet.
 */
struct ImportRule {
  /// URL of the imported stylesheet, exactly as authored (unresolved).
  RcString url;

  /// Source position of the `@` token in the local CSS source.
  FileOffset sourceOffset = FileOffset::EndOfString();

  LayerId layer = kUnlayered;  //!< Optional named or anonymous import layer.
};

/**
 * A CSS stylesheet, which is a list of rules. This is created by the parser, from the \ref
 * CSS::ParseStylesheet() API.
 */
class Stylesheet {
public:
  /// Resource-limit accounting produced while parsing an untrusted stylesheet.
  struct SecurityStats {
    std::size_t rules = 0;
    std::size_t declarations = 0;
    std::size_t componentValues = 0;
    bool rejected = false;
  };

  /// Maximum declarations retained across one stylesheet.
  static constexpr std::size_t kMaximumDeclarations = 16 * 1024;
  /// Maximum distinct layer nodes and path depth in one sheet.
  static constexpr std::size_t kMaximumLayers = 4096;
  static constexpr std::size_t kMaximumLayerDepth = 32;

  /// Default constructor.
  Stylesheet() = default;

  /**
   * Construct a stylesheet from a list of rules.
   *
   * @param rules List of rules, ownership is taken.
   * @param fontFaces Optional list of `@font-face` declarations, ownership is taken.
   */
  explicit Stylesheet(std::vector<SelectorRule>&& rules, std::vector<FontFace>&& fontFaces = {})
      : Stylesheet(std::move(rules), std::move(fontFaces), SecurityStats{}) {}

  /** Construct a stylesheet with parser resource-limit accounting. */
  Stylesheet(std::vector<SelectorRule>&& rules, std::vector<FontFace>&& fontFaces,
             SecurityStats securityStats)
      : rules_(std::move(rules)), fontFaces_(std::move(fontFaces)), securityStats_(securityStats) {}

  /** Construct a stylesheet with retained `@import` rules and parser accounting. */
  Stylesheet(std::vector<SelectorRule>&& rules, std::vector<FontFace>&& fontFaces,
             std::vector<ImportRule>&& imports, SecurityStats securityStats,
             std::shared_ptr<const StylesheetLayers> layers = nullptr)
      : rules_(std::move(rules)),
        fontFaces_(std::move(fontFaces)),
        imports_(std::move(imports)),
        layers_(std::move(layers)),
        securityStats_(securityStats) {}

  // Copyable and moveable.
  /// Copy constructor.
  Stylesheet(const Stylesheet&) = default;
  /// Copy assignment operator.
  Stylesheet& operator=(const Stylesheet&) = default;
  /// Move constructor.
  Stylesheet(Stylesheet&&) noexcept = default;
  /// Move assignment operator.
  Stylesheet& operator=(Stylesheet&&) noexcept = default;

  /// Destructor.
  ~Stylesheet() = default;

  /**
   * Get the list of rules in this stylesheet.
   */
  std::span<const SelectorRule> rules() const UTILS_LIFETIME_BOUND { return rules_; }

  /**
   * Get the list of `@font-face` rules in this stylesheet.
   */
  std::span<const FontFace> fontFaces() const UTILS_LIFETIME_BOUND { return fontFaces_; }

  /**
   * Get mutable access to the `@font-face` rules, for example to resolve their relative source
   * URLs against the URL a stylesheet was loaded from.
   */
  std::span<FontFace> mutableFontFaces() UTILS_LIFETIME_BOUND { return fontFaces_; }

  /**
   * Get the unconditional `@import` rules of this stylesheet, in source order. Imported rules
   * precede this stylesheet's own \ref rules in the cascade.
   */
  std::span<const ImportRule> imports() const UTILS_LIFETIME_BOUND { return imports_; }

  /// Layer syntax in source order. A null value leaves ordinary imports in imports() order.
  const StylesheetLayers* layers() const UTILS_LIFETIME_BOUND { return layers_.get(); }

  /// Return resource-limit accounting from parsing this stylesheet.
  const SecurityStats& securityStats() const { return securityStats_; }

  /**
   * Output a human-readable representation of the stylesheet to a stream.
   *
   * @param os Output stream.
   * @param stylesheet Stylesheet to output.
   */
  friend std::ostream& operator<<(std::ostream& os, const Stylesheet& stylesheet) {
    for (const auto& rule : stylesheet.rules()) {
      os << rule << "\n";
    }
    return os;
  }

private:
  std::vector<SelectorRule> rules_;
  std::vector<FontFace> fontFaces_;
  std::vector<ImportRule> imports_;
  std::shared_ptr<const StylesheetLayers> layers_;
  SecurityStats securityStats_;
};

}  // namespace donner::css
