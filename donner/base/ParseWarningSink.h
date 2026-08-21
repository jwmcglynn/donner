#pragma once
/// @file

#include <concepts>
#include <cstddef>
#include <type_traits>
#include <vector>

#include "donner/base/ParseDiagnostic.h"

namespace donner {

/**
 * Collects parse warnings during parsing. Always safe to call `add()` on---when disabled,
 * warnings are silently dropped without invoking the factory callable, implicitly avoiding
 * string formatting overhead.
 *
 * Replaces the `std::vector<ParseDiagnostic>* outWarnings` pattern.
 *
 * Usage:
 * @code
 * // The lambda is only invoked if the sink is enabled---no formatting overhead when disabled.
 * sink.add([&] {
 *   return ParseDiagnostic::Warning(
 *       RcString::fromFormat("Unknown attribute '{}'", std::string_view(name)), range);
 * });
 * @endcode
 */
class ParseWarningSink {
public:
  /// Maximum warning objects retained by one sink.
  static constexpr std::size_t kMaximumWarnings = 1024;

  /// Maximum aggregate warning-reason bytes retained by one sink.
  static constexpr std::size_t kMaximumWarningBytes = 1024 * 1024;

  /// Construct a sink that collects warnings.
  ParseWarningSink() = default;

  /// Construct a disabled sink that discards all warnings (no-op).
  static ParseWarningSink Disabled() {
    ParseWarningSink sink;
    sink.enabled_ = false;
    return sink;
  }

  /// Returns true if the sink is enabled (will store warnings).
  bool isEnabled() const { return enabled_; }

  /**
   * Add a warning via a factory callable. The callable is only invoked when the sink is enabled,
   * implicitly avoiding formatting overhead when warnings are disabled.
   *
   * @tparam Factory A callable returning ParseDiagnostic.
   */
  template <typename Factory>
    requires std::invocable<Factory> && std::same_as<std::invoke_result_t<Factory>, ParseDiagnostic>
  void add(Factory&& factory) {
    if (enabled_ && !resourceLimitExceeded_) {
      if (warnings_.size() >= kMaximumWarnings) {
        resourceLimitExceeded_ = true;
        return;
      }
      tryAdd(std::forward<Factory>(factory)());
    }
  }

  /// Add a pre-constructed warning (for cases where the diagnostic is already built).
  void add(ParseDiagnostic&& warning) {
    if (enabled_ && !resourceLimitExceeded_) {
      tryAdd(std::move(warning));
    }
  }

  /// Access the collected warnings.
  const std::vector<ParseDiagnostic>& warnings() const { return warnings_; }

  /// Returns true if any warnings have been added.
  bool hasWarnings() const { return !warnings_.empty(); }

  /// Returns true if warnings were discarded to enforce a resource limit.
  bool resourceLimitExceeded() const { return resourceLimitExceeded_; }

  /// Aggregate bytes retained for warning messages.
  std::size_t warningBytes() const { return warningBytes_; }

  /// Merge all warnings from another sink into this one.
  void merge(ParseWarningSink&& other) {
    if (enabled_) {
      for (ParseDiagnostic& warning : other.warnings_) {
        add(std::move(warning));
      }
      resourceLimitExceeded_ |= other.resourceLimitExceeded_;
    }
  }

  /**
   * Merge warnings from a subparser, remapping source ranges using the given parent offset.
   *
   * Each warning's range.start and range.end are remapped via addParentOffset(parentOffset),
   * translating them from the subparser's local coordinate space to the parent's.
   *
   * @param other Subparser's warning sink to merge from.
   * @param parentOffset The offset of the subparser's input within the parent input.
   */
  void mergeFromSubparser(ParseWarningSink&& other, FileOffset parentOffset) {
    if (enabled_) {
      for (auto& warning : other.warnings_) {
        warning.range.start = warning.range.start.addParentOffset(parentOffset);
        warning.range.end = warning.range.end.addParentOffset(parentOffset);
        add(std::move(warning));
      }
      resourceLimitExceeded_ |= other.resourceLimitExceeded_;
    }
  }

private:
  void tryAdd(ParseDiagnostic&& warning) {
    if (warnings_.size() >= kMaximumWarnings ||
        warning.reason.size() > kMaximumWarningBytes - warningBytes_) {
      resourceLimitExceeded_ = true;
      return;
    }
    warningBytes_ += warning.reason.size();
    warnings_.push_back(std::move(warning));
  }

  std::vector<ParseDiagnostic> warnings_;
  std::size_t warningBytes_ = 0;
  bool enabled_ = true;
  bool resourceLimitExceeded_ = false;
};

}  // namespace donner
