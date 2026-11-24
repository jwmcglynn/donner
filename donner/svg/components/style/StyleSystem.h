#pragma once
/// @file

#include <memory>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/FileOffset.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/css/Specificity.h"
#include "donner/css/selectors/SelectorMatchOptions.h"
#include "donner/svg/components/DocumentResourceFamilyBudget.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/ExternalUseStyleContext.h"
#include "donner/svg/components/style/StyleCascadeMetrics.h"

namespace donner::css {
struct Declaration;
}

namespace donner::svg::components {

class AnimationContextualWorkBudget;
struct MatchedPropertiesKey;
struct StyleComponent;

/**
 * Test-only switch: while a registry's context holds this, the cascade and
 * \ref StyleSystem::collectMatchedStyleRules test every rule of every applicable stylesheet
 * against every element instead of only the rule index's candidates. Differential tests compare
 * the two; nothing else changes, so any difference is a rule the index wrongly skipped.
 */
struct UnindexedCascadeForTesting {};

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
  /// Default maximum path bytes external-use measurement retains in one frame.
  static constexpr std::size_t kMaximumExternalMeasureGeometryBytes = 32 * 1024 * 1024;

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
    /// Maximum entities laid out per frame to measure external-use content for host effects.
    std::size_t externalMeasureEntities = kMaximumRuleElementMatches;
    /// Maximum path bytes retained per frame by external-use content measured for host effects.
    std::size_t externalMeasureGeometryBytes = kMaximumExternalMeasureGeometryBytes;
  };

  /**
   * Construct a style budget with default limits.
   * @param family Optional shared document-family envelope for retained style bytes.
   */
  explicit StyleResourceBudget(std::shared_ptr<DocumentResourceFamilyBudget> family = nullptr)
      : retainedStyleBytes_(std::make_shared<RetainedStyleBytes>(kMaximumComplexPropertyBytes,
                                                                 std::move(family))) {}
  /**
   * Construct a style budget with caller-supplied limits.
   * @param limits Cascade, traversal, and retained-property ceilings.
   * @param family Optional shared document-family envelope for retained style bytes.
   */
  explicit StyleResourceBudget(Limits limits,
                               std::shared_ptr<DocumentResourceFamilyBudget> family = nullptr)
      : limits_(limits),
        selectorTraversal_(limits.selectorTraversalSteps),
        retainedStyleBytes_(
            std::make_shared<RetainedStyleBytes>(limits.complexPropertyBytes, std::move(family))) {}

  StyleResourceBudget(const StyleResourceBudget&) = delete;
  StyleResourceBudget& operator=(const StyleResourceBudget&) = delete;
  /**
   * Transfer counters and retained-property reservations from another budget.
   * Only move a budget when no OwnerScope or ActiveRegistryScope refers to it.
   * @param other Budget whose family charge moves to this instance.
   */
  StyleResourceBudget(StyleResourceBudget&& other) noexcept
      : contextualAnimationWork_(std::move(other.contextualAnimationWork_)),
        externalInstanceWork_(other.externalInstanceWork_),
        externalMeasureWork_(other.externalMeasureWork_),
        externalMeasureGeometryBytes_(other.externalMeasureGeometryBytes_),
        externalMeasureGeometryComparisons_(other.externalMeasureGeometryComparisons_),
        externalMeasureGeometryExhausted_(other.externalMeasureGeometryExhausted_),
        externalResourceWarning_(other.externalResourceWarning_),
        aggregateWork_(other.aggregateWork_),
        limits_(other.limits_),
        selectorTraversal_(std::move(other.selectorTraversal_)),
        ruleElementMatches_(other.ruleElementMatches_),
        declarationApplications_(other.declarationApplications_),
        declarationComponentWork_(other.declarationComponentWork_),
        declarationByteWork_(other.declarationByteWork_),
        rejected_(other.rejected_),
        retainedStyleBytes_(std::move(other.retainedStyleBytes_)) {
    other.aggregateWork_ = nullptr;
    other.externalInstanceWork_ = 0;
    other.externalMeasureWork_ = 0;
    other.externalMeasureGeometryBytes_ = 0;
    other.externalMeasureGeometryComparisons_ = 0;
    other.externalMeasureGeometryExhausted_ = false;
  }
  StyleResourceBudget& operator=(StyleResourceBudget&&) = delete;

  /** Shared owner scope; nested recording and fallback work preserves the current allowance. */
  class OwnerScope {
  public:
    /**
     * Begin one logical render operation, resetting only its outermost scope.
     * @param budget Shared allowance for serialized owner-frame traversal.
     */
    explicit OwnerScope(StyleResourceBudget& budget)
        : budget_(budget), previous_(std::exchange(budget.ownerActive_, true)) {
      if (!previous_) {
        budget_.reset();
        budget_.resetContextualAnimationWork();
      }
    }
    ~OwnerScope() { budget_.ownerActive_ = previous_; }
    OwnerScope(const OwnerScope&) = delete;
    OwnerScope& operator=(const OwnerScope&) = delete;

  private:
    StyleResourceBudget& budget_;
    bool previous_;
  };

  /** Keep mutable document storage active throughout preparation, drawing and restoration. */
  class ActiveRegistryScope {
  public:
    /**
     * Mark a registry active without removing an enclosing scope's registration.
     * @param budget Shared traversal identity context.
     * @param registry Mutable storage protected by the caller's document access.
     */
    ActiveRegistryScope(StyleResourceBudget& budget, Registry& registry);
    ~ActiveRegistryScope();
    ActiveRegistryScope(const ActiveRegistryScope&) = delete;
    ActiveRegistryScope& operator=(const ActiveRegistryScope&) = delete;

  private:
    StyleResourceBudget& budget_;
    Registry* registry_;
    bool inserted_;
    bool hadContext_ = false;
    std::shared_ptr<AnimationContextualWorkBudget> boundContextualWork_;
    std::shared_ptr<AnimationContextualWorkBudget> previousContext_;
    std::shared_ptr<AnimationContextualWorkBudget> previousBudget_;
  };

  /**
   * Return whether mutable storage is already on this render operation's traversal stack.
   * @param registry Storage considered for child admission.
   */
  bool isRegistryActive(const Registry& registry) const {
    return activeRegistries_.contains(&registry);
  }

  /// Admit bounded entity work for one external-use preparation against the frame allowance.
  bool reserveExternalInstanceWork(std::size_t entities) {
    if (rejected_ || entities > limits_.ruleElementMatches - externalInstanceWork_) {
      rejected_ = true;
      return false;
    }
    externalInstanceWork_ += entities;
    return true;
  }

  /// External-use instance work admitted in the current frame.
  [[nodiscard]] std::size_t externalInstanceWork() const { return externalInstanceWork_; }

  /**
   * Admit bounded entity work for laying out one external-use occurrence to measure its bounds
   * for a host effect, against a frame allowance of its own. Measuring recurses into the
   * occurrence's own external uses, so without an allowance a family of documents that each use
   * the next several times would cost exponential work. A refusal leaves drawing unaffected.
   * @param entities Entities the measured occurrence lays out.
   * @return True when the work fits the remaining measurement allowance.
   */
  bool reserveExternalMeasureWork(std::size_t entities) {
    if (entities > limits_.externalMeasureEntities - externalMeasureWork_) {
      return false;
    }
    externalMeasureWork_ += entities;
    return true;
  }

  /// External-use measurement work admitted in the current frame.
  [[nodiscard]] std::size_t externalMeasureWork() const { return externalMeasureWork_; }

  /**
   * Admit @p bytes of path geometry that external-use measurement retains until the frame ends,
   * against a frame allowance of its own. The first refusal exhausts the allowance for the frame.
   * @param bytes Bytes of one retained path copy.
   * @return True when the bytes fit the remaining allowance.
   */
  bool reserveExternalMeasureGeometryBytes(std::size_t bytes) {
    if (externalMeasureGeometryExhausted_ ||
        bytes > limits_.externalMeasureGeometryBytes - externalMeasureGeometryBytes_) {
      externalMeasureGeometryExhausted_ = true;
      return false;
    }
    externalMeasureGeometryBytes_ += bytes;
    return true;
  }

  /// Path bytes external-use measurement retains in the current frame.
  [[nodiscard]] std::size_t externalMeasureGeometryBytes() const {
    return externalMeasureGeometryBytes_;
  }

  /// Whether a path copy external-use measurement asked to retain did not fit the current frame's
  /// allowance, after which no more are retained.
  [[nodiscard]] bool externalMeasureGeometryExhausted() const {
    return externalMeasureGeometryExhausted_;
  }

  /// Count one full comparison of a measured path against a retained path copy.
  void noteExternalMeasureGeometryComparison() { ++externalMeasureGeometryComparisons_; }

  /// Full path comparisons external-use measurement made in the current frame.
  [[nodiscard]] std::size_t externalMeasureGeometryComparisons() const {
    return externalMeasureGeometryComparisons_;
  }

  /// Return true once per frame when an unsupported inherited resource needs a diagnostic.
  bool reportExternalResourceWarning() { return !std::exchange(externalResourceWarning_, true); }

  /// Set a frame-wide aggregate work allowance while an external-use scope is active.
  void setAggregateWorkBudget(StyleResourceBudget* budget) { aggregateWork_ = budget; }

  /// Reset traversal, match/declaration counters and local refusal; retained bytes remain.
  void reset() {
    selectorTraversal_.reset();
    ruleElementMatches_ = 0;
    declarationApplications_ = 0;
    declarationComponentWork_ = 0;
    declarationByteWork_ = 0;
    rejected_ = false;
    externalInstanceWork_ = 0;
    externalMeasureWork_ = 0;
    externalMeasureGeometryBytes_ = 0;
    externalMeasureGeometryComparisons_ = 0;
    externalMeasureGeometryExhausted_ = false;
    externalResourceWarning_ = false;
  }

  /// Charge one rule-to-element match, or latch rejection when a limit has been reached.
  [[nodiscard]] bool reserveRuleElementMatch() {
    if (rejected_ || selectorTraversal_.rejected() ||
        ruleElementMatches_ >= limits_.ruleElementMatches) {
      rejected_ = true;
      return false;
    }
    if (aggregateWork_ && !aggregateWork_->reserveRuleElementMatch()) {
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
    if (aggregateWork_ &&
        !aggregateWork_->reserveDeclarationApplication(componentCount, sourceBytes)) {
      rejected_ = true;
      return false;
    }
    ++declarationApplications_;
    declarationComponentWork_ += componentCount;
    declarationByteWork_ += sourceBytes;
    return true;
  }

  /** Test a declaration-work sequence without changing counters or refusal state. */
  [[nodiscard]] bool canReserveDeclarationWork(std::size_t applications, std::size_t componentCount,
                                               std::size_t sourceBytes) const {
    if (rejected_ || selectorTraversal_.rejected() ||
        declarationApplications_ > limits_.declarationApplications ||
        declarationComponentWork_ > limits_.declarationComponentWork ||
        declarationByteWork_ > limits_.declarationByteWork ||
        applications > limits_.declarationApplications - declarationApplications_ ||
        componentCount > limits_.declarationComponentWork - declarationComponentWork_ ||
        sourceBytes > limits_.declarationByteWork - declarationByteWork_) {
      return false;
    }
    return !aggregateWork_ ||
           aggregateWork_->canReserveDeclarationWork(applications, componentCount, sourceBytes);
  }

  /** Charge one immutable computed-style record for exactly its lifetime. */
  [[nodiscard]] bool retainComputedStyle(ComputedStyle& style, std::size_t byteCount) {
    if (style.retainedStyleBytes_) {
      return false;
    }
    if (byteCount != 0 && rejected_) {
      return false;
    }
    if (!style.retainBytes(retainedStyleBytes_, byteCount)) {
      rejected_ = true;
      return false;
    }
    return true;
  }

  /// Return whether bytes fit without changing counters or refusal state.
  [[nodiscard]] bool canRetainComputedStyle(std::size_t byteCount) const {
    return byteCount == 0 || (!rejected_ && retainedStyleBytes_->canReserve(byteCount));
  }

  /// Shared account held by every computed-style record created for this document.
  [[nodiscard]] const std::shared_ptr<RetainedStyleBytes>& retainedStyleBytes() const {
    return retainedStyleBytes_;
  }

  /// Return the mutable selector traversal budget for this computation pass.
  [[nodiscard]] css::SelectorTraversalBudget& selectorTraversal() {
    return aggregateWork_ ? aggregateWork_->selectorTraversal() : selectorTraversal_;
  }
  /// Return the selector traversal budget for this computation pass.
  [[nodiscard]] const css::SelectorTraversalBudget& selectorTraversal() const {
    return aggregateWork_ ? aggregateWork_->selectorTraversal() : selectorTraversal_;
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
  [[nodiscard]] std::size_t complexPropertyBytes() const {
    return retainedStyleBytes_->retainedBytes();
  }
  /// Return whether this budget or its selector traversal budget refused work.
  [[nodiscard]] bool rejected() const {
    return rejected_ || selectorTraversal_.rejected() ||
           (aggregateWork_ && aggregateWork_->rejected());
  }
  /// Return the active cascade, traversal, and retained-property ceilings.
  [[nodiscard]] const Limits& limits() const { return limits_; }

private:
  std::shared_ptr<AnimationContextualWorkBudget> contextualAnimationWorkBudget();
  void resetContextualAnimationWork();

  bool ownerActive_ = false;
  std::unordered_set<const Registry*> activeRegistries_;
  std::shared_ptr<AnimationContextualWorkBudget> contextualAnimationWork_;
  std::size_t externalInstanceWork_ = 0;
  std::size_t externalMeasureWork_ = 0;
  std::size_t externalMeasureGeometryBytes_ = 0;
  std::size_t externalMeasureGeometryComparisons_ = 0;
  bool externalMeasureGeometryExhausted_ = false;
  bool externalResourceWarning_ = false;
  StyleResourceBudget* aggregateWork_ = nullptr;
  Limits limits_;
  css::SelectorTraversalBudget selectorTraversal_;
  std::size_t ruleElementMatches_ = 0;
  std::size_t declarationApplications_ = 0;
  std::size_t declarationComponentWork_ = 0;
  std::size_t declarationByteWork_ = 0;
  bool rejected_ = false;
  std::shared_ptr<RetainedStyleBytes> retainedStyleBytes_;
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
  /// Candidate rule positions the per-thread list used by the cascade keeps between elements, in
  /// entries: an element's cascade that needed more releases the list afterwards, so a large
  /// stylesheet does not pin its memory for the life of the thread. Equal to the stylesheet
  /// parser's rule limit, the most candidates one sheet has once duplicates are removed.
  static constexpr std::size_t kRetainedCandidateCapacity = 4096;

  /// Capacity of the calling thread's candidate list, in entries, for tests of the retention cap.
  static std::size_t CandidateListCapacityForTesting();

  /**
   * Compute the style for the given entity handle, applying the CSS cascade and inheritance rules.
   *
   * @param handle Entity handle to compute the style for
   * @param warningSink Containing any warnings found
   * @returns Computed style component for the entity
   */
  const ComputedStyleComponent& computeStyle(EntityHandle handle, ParseWarningSink& warningSink);

  /// What \ref computeAllStyles does with existing computed styles when the whole document needs
  /// restyling (\ref RenderTreeState::needsFullStyleRecompute on a built document).
  enum class FullRestyle {
    /// Discard every computed style, then compute the whole document.
    DiscardAndRecompute,
    /// Compute only entities without a computed style. For a caller that already discarded the
    /// stale styles with \ref discardComputedStylesForFullRestyle and runs several passes over one
    /// rebuild, so each entity is cascaded once.
    ComputeMissing,
  };

  /**
   * Computes the style for all entities in the registry.
   *
   * @param registry Registry to compute the styles, used to query for all entities in the tree.
   * @param warningSink Containing any warnings found
   * @param fullRestyle What a whole-document restyle does with existing computed styles.
   */
  void computeAllStyles(Registry& registry, ParseWarningSink& warningSink,
                        FullRestyle fullRestyle = FullRestyle::DiscardAndRecompute);

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
   * Only rules of each `<style>` element's own stylesheet are reported; rules loaded through
   * `@import` have no document source and are not indexed by \ref MatchedStyleRule::ruleIndex.
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
   * Discard every cached computed style and its retained style budget, for changes that affect
   * selector matching across the whole document, such as reloaded stylesheet imports. Unlike
   * \ref RenderTreeState::needsFullStyleRecompute, this also covers styles computed by queries
   * before the render tree was first built.
   *
   * @param registry Registry containing the document.
   */
  static void discardComputedStyles(Registry& registry);

  /**
   * Discard every computed style and its retained style budget to restyle the whole document,
   * counted in \ref StyleCascadeCounters::fullClears. Style passes that follow with
   * \ref FullRestyle::ComputeMissing then compute every entity again, once.
   *
   * @param registry Registry containing the document.
   */
  static void discardComputedStylesForFullRestyle(Registry& registry);

  /// Inherit fallback values from an external-use computed parent without transferring specificity.
  /// Resolved ordinary host paint, retained only during the guarded occurrence.
  static ResolvedPaintServer externalHostPaint(Registry& registry, bool stroke);
  /// Record a refused inherited resource for the occurrence's static diagnostic.
  static void rejectExternalHostResource(Registry& registry);
  /// Local paints alone create child-registry resource shadow trees.
  static std::optional<PaintServer> localPaintForInstantiation(const ComputedStyleComponent& style,
                                                               bool stroke);

  /// Apply inherited values from an external parent in place while retaining local declaration
  /// precedence.
  /// @param properties Declarations in the referenced document, which receive the external-parent
  ///   fallback values.
  /// @param parent Computed declarations inherited from the referencing element.
  static void inheritExternalParent(PropertyRegistry& properties, const PropertyRegistry& parent);

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
  struct PendingStyleDeclarations;

  /// Compute the style of every entity flagged \ref DirtyFlagsComponent::Style whose computed
  /// style was removed.
  void computeDirtyStyles(Registry& registry, ParseWarningSink& warningSink);
  /// Compute the style of every tree entity without a computed style.
  void computeMissingStyles(Registry& registry, ParseWarningSink& warningSink);
  void applyStylesheetRules(const Registry& registry, Entity treeEntity, Entity dataEntity,
                            StyleResourceBudget& styleBudget, StyleCascadeCounters& counters,
                            MatchedPropertiesKey& cacheKey, const StyleComponent* local,
                            ParseWarningSink& warningSink, PendingStyleDeclarations& declarations);
  void inheritComputedParent(EntityHandle handle, Entity parent, PropertyRegistry& properties,
                             ComputedStyleComponent& computed, ParseWarningSink& warningSink);
  static void resolveComputedParentFonts(EntityHandle handle, Entity parent,
                                         PropertyRegistry& properties);
  void inheritProperties(Registry& registry, Entity parent, PropertyRegistry& properties,
                         ParseWarningSink& warningSink);
  static void resolveRelativeFontProperties(Registry& registry, Entity parent,
                                            PropertyRegistry& properties);
  void computePropertiesInto(EntityHandle handle, ComputedStyleComponent& computedStyle,
                             ParseWarningSink& warningSink);
};

}  // namespace donner::svg::components
