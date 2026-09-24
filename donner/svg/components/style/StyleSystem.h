#pragma once
/// @file

#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/FileOffset.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/css/Specificity.h"
#include "donner/css/selectors/SelectorMatchOptions.h"
#include "donner/svg/components/DocumentResourceFamilyBudget.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"

namespace donner::svg::components {

/** Aggregate stylesheet cascade and selector traversal budget for one style-computation pass. */
class StyleResourceBudget {
public:
  /// Default maximum rule-to-element matches in one computation pass.
  static constexpr std::size_t kMaximumRuleElementMatches = 1024 * 1024;
  /// Default maximum declaration applications in one pass.
  static constexpr std::size_t kMaximumDeclarationApplications = 1024 * 1024;
  /// Default maximum aggregate declaration component work units.
  static constexpr std::size_t kMaximumDeclarationComponentWork = 16 * 1024 * 1024;
  /// Default maximum aggregate declaration source bytes charged.
  static constexpr std::size_t kMaximumDeclarationByteWork = 16 * 1024 * 1024;
  /// Default maximum retained complex-property bytes.
  static constexpr std::size_t kMaximumComplexPropertyBytes = 64 * 1024 * 1024;

  /// Independent cascade, traversal, and retained-property limits.
  struct Limits {
    /// Maximum rule-to-element matches.
    std::size_t ruleElementMatches = kMaximumRuleElementMatches;
    /// Maximum declaration applications.
    std::size_t declarationApplications = kMaximumDeclarationApplications;
    /// Maximum aggregate declaration component work units.
    std::size_t declarationComponentWork = kMaximumDeclarationComponentWork;
    /// Maximum aggregate declaration source bytes charged.
    std::size_t declarationByteWork = kMaximumDeclarationByteWork;
    /// Maximum retained complex-property bytes for this budget.
    std::size_t complexPropertyBytes = kMaximumComplexPropertyBytes;
    /// Maximum selector traversal steps.
    std::size_t selectorTraversalSteps = css::SelectorTraversalBudget::kMaximumSteps;
  };

  /**
   * Construct a style budget with default limits.
   * @param family Optional shared document-family envelope for retained style bytes.
   */
  explicit StyleResourceBudget(std::shared_ptr<DocumentResourceFamilyBudget> family = nullptr)
      : family_(std::move(family)) {}
  /**
   * Construct a style budget with caller-supplied limits.
   * @param limits Cascade, traversal, and retained-property ceilings.
   * @param family Optional shared document-family envelope for retained style bytes.
   */
  explicit StyleResourceBudget(Limits limits,
                               std::shared_ptr<DocumentResourceFamilyBudget> family = nullptr)
      : limits_(limits),
        selectorTraversal_(limits.selectorTraversalSteps),
        family_(std::move(family)) {}
  ~StyleResourceBudget() {
    if (family_) {
      family_->release(DocumentResourceFamilyBudget::Kind::ComputedStyle, complexPropertyBytes_);
    }
  }

  StyleResourceBudget(const StyleResourceBudget&) = delete;
  StyleResourceBudget& operator=(const StyleResourceBudget&) = delete;
  /**
   * Transfer counters and retained-property reservations from another budget.
   * @param other Budget whose family charge moves to this instance.
   */
  StyleResourceBudget(StyleResourceBudget&& other) noexcept
      : limits_(other.limits_),
        selectorTraversal_(std::move(other.selectorTraversal_)),
        ruleElementMatches_(other.ruleElementMatches_),
        declarationApplications_(other.declarationApplications_),
        declarationComponentWork_(other.declarationComponentWork_),
        declarationByteWork_(other.declarationByteWork_),
        complexPropertyBytes_(other.complexPropertyBytes_),
        rejected_(other.rejected_),
        family_(std::move(other.family_)),
        reservations_(std::move(other.reservations_)) {
    other.complexPropertyBytes_ = 0;
  }
  StyleResourceBudget& operator=(StyleResourceBudget&&) = delete;

  /// Reset traversal, match/declaration counters and local refusal; retained bytes remain.
  void reset() {
    selectorTraversal_.reset();
    ruleElementMatches_ = 0;
    declarationApplications_ = 0;
    declarationComponentWork_ = 0;
    declarationByteWork_ = 0;
    rejected_ = false;
  }

  /// Charge one rule-to-element match, or latch rejection when a limit has been reached.
  [[nodiscard]] bool reserveRuleElementMatch() {
    if (rejected_ || selectorTraversal_.rejected() ||
        ruleElementMatches_ >= limits_.ruleElementMatches) {
      rejected_ = true;
      return false;
    }
    ++ruleElementMatches_;
    return true;
  }

  /**
   * Charge one declaration application and its associated component and source work.
   * @param componentCount Component work units for this application.
   * @param sourceBytes Source bytes charged for this application.
   * @return True when all charges fit; false after a prior or current refusal.
   */
  [[nodiscard]] bool reserveDeclarationApplication(std::size_t componentCount = 0,
                                                   std::size_t sourceBytes = 0) {
    if (rejected_ || selectorTraversal_.rejected() ||
        declarationApplications_ >= limits_.declarationApplications ||
        declarationComponentWork_ > limits_.declarationComponentWork ||
        componentCount > limits_.declarationComponentWork - declarationComponentWork_ ||
        declarationByteWork_ > limits_.declarationByteWork ||
        sourceBytes > limits_.declarationByteWork - declarationByteWork_) {
      rejected_ = true;
      return false;
    }
    ++declarationApplications_;
    declarationComponentWork_ += componentCount;
    declarationByteWork_ += sourceBytes;
    return true;
  }

  /**
   * Set an entity's retained complex-property charge, releasing bytes on reductions.
   * @param entity Entity whose property charge changes.
   * @param byteCount New retained byte count for this entity.
   * @return True for a reduction or an admitted increase; false for a refused increase.
   */
  [[nodiscard]] bool reserveComplexPropertyBytes(Entity entity, std::size_t byteCount) {
    const std::size_t previous = reservations_[entity];
    if (byteCount <= previous) {
      const std::size_t released = previous - byteCount;
      complexPropertyBytes_ -= released;
      reservations_[entity] = byteCount;
      if (family_) {
        family_->release(DocumentResourceFamilyBudget::Kind::ComputedStyle, released);
      }
      return true;
    }

    const std::size_t additional = byteCount - previous;
    if (rejected_ || complexPropertyBytes_ > limits_.complexPropertyBytes ||
        additional > limits_.complexPropertyBytes - complexPropertyBytes_ ||
        (family_ &&
         !family_->reserve(DocumentResourceFamilyBudget::Kind::ComputedStyle, additional))) {
      rejected_ = true;
      return false;
    }
    complexPropertyBytes_ += additional;
    reservations_[entity] = byteCount;
    return true;
  }

  /**
   * Release one entity's retained complex-property bytes, if it has a reservation.
   * @param entity Entity whose reservation is removed.
   */
  void release(Entity entity) {
    const auto it = reservations_.find(entity);
    if (it == reservations_.end()) {
      return;
    }
    const std::size_t bytes = it->second;
    complexPropertyBytes_ -= bytes;
    reservations_.erase(it);
    if (family_) {
      family_->release(DocumentResourceFamilyBudget::Kind::ComputedStyle, bytes);
    }
  }

  /// Release all retained complex-property bytes without resetting other counters.
  void releaseAll() {
    if (family_) {
      family_->release(DocumentResourceFamilyBudget::Kind::ComputedStyle, complexPropertyBytes_);
    }
    reservations_.clear();
    complexPropertyBytes_ = 0;
  }

  /// Return the mutable selector traversal budget for this computation pass.
  [[nodiscard]] css::SelectorTraversalBudget& selectorTraversal() { return selectorTraversal_; }
  /// Return the selector traversal budget for this computation pass.
  [[nodiscard]] const css::SelectorTraversalBudget& selectorTraversal() const {
    return selectorTraversal_;
  }
  /// Return the number of rule-to-element matches charged.
  [[nodiscard]] std::size_t ruleElementMatches() const { return ruleElementMatches_; }
  /// Return the number of declaration applications charged.
  [[nodiscard]] std::size_t declarationApplications() const { return declarationApplications_; }
  /// Return aggregate declaration component work units charged.
  [[nodiscard]] std::size_t declarationComponentWork() const { return declarationComponentWork_; }
  /// Return aggregate declaration source bytes charged.
  [[nodiscard]] std::size_t declarationByteWork() const { return declarationByteWork_; }
  /// Return retained complex-property bytes charged to this budget.
  [[nodiscard]] std::size_t complexPropertyBytes() const { return complexPropertyBytes_; }
  /// Return whether this budget or its selector traversal budget refused work.
  [[nodiscard]] bool rejected() const { return rejected_ || selectorTraversal_.rejected(); }
  /// Return the active cascade, traversal, and retained-property ceilings.
  [[nodiscard]] const Limits& limits() const { return limits_; }

private:
  Limits limits_;
  css::SelectorTraversalBudget selectorTraversal_;
  std::size_t ruleElementMatches_ = 0;
  std::size_t declarationApplications_ = 0;
  std::size_t declarationComponentWork_ = 0;
  std::size_t declarationByteWork_ = 0;
  std::size_t complexPropertyBytes_ = 0;
  bool rejected_ = false;
  std::shared_ptr<DocumentResourceFamilyBudget> family_;
  std::unordered_map<Entity, std::size_t> reservations_;
};

/// Diagnostic record for one stylesheet rule that matched an element.
struct MatchedStyleRule {
  Entity stylesheetEntity = entt::null;  ///< Entity carrying the matched stylesheet.
  std::size_t ruleIndex = 0;             ///< Index of the matched rule within the stylesheet.
  std::size_t selectorEntryIndex = 0;    ///< Index of the matched selector-list entry.
  css::Specificity specificity;          ///< Specificity used for cascading this rule.
  bool isUserAgentStylesheet = false;    ///< True when this came from the built-in UA stylesheet.
  std::optional<SourceRange> ruleSourceRange;        ///< Matched rule range in SVG source.
  std::optional<SourceRange> selectorSourceRange;    ///< Matched selector branch in SVG source.
  std::vector<SourceRange> declarationSourceRanges;  ///< Declaration ranges in SVG source.

  /// Equality operator.
  bool operator==(const MatchedStyleRule& other) const = default;
};

/// Stylesheet rule found at a document source offset.
struct StyleRuleAtSourceOffset {
  Entity stylesheetEntity = entt::null;           ///< Entity carrying the stylesheet.
  std::size_t ruleIndex = 0;                      ///< Index of the source rule.
  std::optional<std::size_t> selectorEntryIndex;  ///< Selector-list entry containing the offset.
  SourceRange ruleSourceRange{FileOffset::Offset(0),
                              FileOffset::Offset(0)};  ///< Rule range in SVG source.
  SourceRange selectorSourceRange{FileOffset::Offset(0),
                                  FileOffset::Offset(0)};  ///< Selector range in SVG source.

  /// Equality operator.
  bool operator==(const StyleRuleAtSourceOffset& other) const = default;
};

/**
 * Computes stylesheet information for elements, applying the CSS cascade and inheritance rules.
 *
 * @ingroup ecs_systems
 * @see https://www.w3.org/TR/SVG2/shapes.html
 */
class StyleSystem {
public:
  /**
   * Compute the style for the given entity handle, applying the CSS cascade and inheritance rules.
   *
   * @param handle Entity handle to compute the style for
   * @param warningSink Containing any warnings found
   * @returns Computed style component for the entity
   */
  const ComputedStyleComponent& computeStyle(EntityHandle handle, ParseWarningSink& warningSink);

  /**
   * Computes the style for all entities in the registry.
   *
   * @param registry Registry to compute the styles, used to query for all entities in the tree.
   * @param warningSink Containing any warnings found
   */
  void computeAllStyles(Registry& registry, ParseWarningSink& warningSink);

  /**
   * Computes the style for the given entities in the registry.
   *
   * @param registry Registry containing the entities
   * @param entities Entities to compute
   * @param warningSink Containing any warnings found
   */
  void computeStylesFor(Registry& registry, std::span<const Entity> entities,
                        ParseWarningSink& warningSink);

  /**
   * Update the style attribute on an element, merging new declarations with existing ones.
   *
   * Declarations in \p style override existing declarations with the same property name.
   * The merged result is written back to the `style` attribute and the PropertyRegistry is updated.
   *
   * @param handle Entity handle to update.
   * @param style CSS style string to merge, e.g. "fill: red; opacity: 0.5".
   */
  void updateStyle(EntityHandle handle, std::string_view style);

  /**
   * Collect stylesheet rules that match \p handle using the same selector matching and specificity
   * adjustments used by \ref computeStyle.
   *
   * @param handle Entity handle to trace stylesheet matches for.
   * @return Matched stylesheet rules in cascade scan order.
   */
  std::vector<MatchedStyleRule> collectMatchedStyleRules(EntityHandle handle) const;

  /**
   * Find the author stylesheet rule at \p documentSourceOffset.
   *
   * If the offset is inside one selector-list entry, \ref
   * StyleRuleAtSourceOffset::selectorEntryIndex is set so callers can focus that selector branch.
   * If the offset is elsewhere inside the rule, such as a declaration block, the entry is unset and
   * the whole selector list is considered active.
   *
   * @param registry SVG document registry to inspect.
   * @param documentSourceOffset Offset in the SVG document source.
   * @return Stylesheet rule at \p documentSourceOffset, or \c std::nullopt.
   */
  std::optional<StyleRuleAtSourceOffset> findStyleRuleAtSourceOffset(
      Registry& registry, std::size_t documentSourceOffset) const;

  /**
   * Invalidate the computed style for a given entity.
   *
   * @param handle Entity handle to invalidate
   */
  void invalidateComputed(EntityHandle handle);

  /**
   * Returns true if any stylesheet loaded into \p registry contains an attribute selector that
   * could match an attribute with the given local name.
   *
   * Writing such an attribute changes selector matching, and can do so for elements other than
   * the one written (sibling combinators, descendant selectors), which per-entity dirty flags do
   * not track. Callers use this to decide whether an attribute write needs a whole-tree restyle.
   *
   * @param registry Document registry.
   * @param localName Attribute local name that is about to be written.
   */
  bool anyStylesheetUsesAttributeInSelector(Registry& registry, std::string_view localName) const;

  /**
   * Invalidate the full style and reparse attributes.
   *
   * @param handle Entity handle to invalidate
   */
  void invalidateAll(EntityHandle handle);

private:
  void applyStylesheetRules(Registry& registry, Entity treeEntity, Entity dataEntity,
                            PropertyRegistry& properties, StyleResourceBudget& styleBudget,
                            ParseWarningSink& warningSink);
  PropertyRegistry inheritProperties(Registry& registry, Entity parent, PropertyRegistry properties,
                                     ParseWarningSink& warningSink);
  static void resolveRelativeFontProperties(Registry& registry, Entity parent,
                                            PropertyRegistry& properties);
  void computePropertiesInto(EntityHandle handle, ComputedStyleComponent& computedStyle,
                             ParseWarningSink& warningSink);
};

}  // namespace donner::svg::components
