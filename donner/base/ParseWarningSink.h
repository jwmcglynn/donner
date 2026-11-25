#pragma once
/// @file

#include <algorithm>
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

  /// Construct an enabled sink with a smaller count and reason-byte allowance.
  static ParseWarningSink Bounded(std::size_t count, std::size_t bytes) {
    ParseWarningSink sink;
    sink.countLimit_ = std::min(count, kMaximumWarnings);
    sink.byteLimit_ = std::min(bytes, kMaximumWarningBytes);
    return sink;
  }

  /// Deliver each admitted diagnostic to a synchronous consumer instead of retaining a vector.
  /// The consumer borrows its argument for this call and returns false if its own allowance is
  /// full.
  static ParseWarningSink Streaming(std::size_t count, std::size_t bytes, void* context,
                                    bool (*consume)(void*, const ParseDiagnostic&)) {
    ParseWarningSink sink = Bounded(count, bytes);
    sink.context_ = context;
    sink.consume_ = consume;
    return sink;
  }

  /// A fresh buffer restricted to what this destination can still retain.
  ParseWarningSink remainingAllowance() const {
    return !enabled_ || resourceLimitExceeded_
               ? Disabled()
               : Bounded(countLimit_ - retainedCount(), byteLimit_ - warningBytes_);
  }

  /// Number of diagnostics retained in the vector or delivered to a streaming consumer.
  std::size_t retainedCount() const { return consume_ ? delivered_ : warnings_.size(); }

  /// Returns true if the sink is enabled (will store warnings).
  bool isEnabled() const { return enabled_; }

  /**
   * Add a warning via a factory callable. The callable is only invoked when the sink is enabled
   * and has not reached a resource limit, implicitly avoiding formatting overhead when warnings
   * are disabled. Once \ref kMaximumWarnings or \ref kMaximumWarningBytes would be exceeded, this
   * and every later warning is dropped and \ref resourceLimitExceeded returns true.
   *
   * @tparam Factory A callable returning ParseDiagnostic.
   * @param factory Callable that builds the warning.
   */
  template <typename Factory>
    requires std::invocable<Factory> && std::same_as<std::invoke_result_t<Factory>, ParseDiagnostic>
  void add(Factory&& factory) {
    ++warningAttempts_;
    if (enabled_ && !resourceLimitExceeded_) {
      if (retainedCount() >= countLimit_) {
        resourceLimitExceeded_ = true;
        return;
      }
      tryAdd(std::forward<Factory>(factory)());
    }
  }

  /// Add a pre-constructed warning (for cases where the diagnostic is already built).
  void add(ParseDiagnostic&& warning) {
    ++warningAttempts_;
    if (enabled_ && !resourceLimitExceeded_) {
      tryAdd(std::move(warning));
    }
  }

  /// Access warnings collected in this sink. A streaming sink delivers them to its consumer.
  const std::vector<ParseDiagnostic>& warnings() const { return warnings_; }

  /// Returns true if any warnings have been added.
  bool hasWarnings() const { return retainedCount() != 0; }

  /// Number of submitted warnings, including those sent to a disabled or saturated sink.
  std::size_t warningAttempts() const { return warningAttempts_; }

  /// Returns true if warnings were discarded to enforce a resource limit.
  bool resourceLimitExceeded() const { return resourceLimitExceeded_; }

  /// Aggregate bytes retained for warning messages.
  std::size_t warningBytes() const { return warningBytes_; }

  /**
   * Remap stored source ranges without changing warning text, severity, or admission accounting.
   *
   * @param mapper Callable taking a source range and returning its replacement range.
   */
  template <typename Mapper>
    requires std::invocable<Mapper&, const SourceRange&> &&
             std::same_as<std::invoke_result_t<Mapper&, const SourceRange&>, SourceRange>
  void mapSourceRanges(Mapper&& mapper) {
    for (ParseDiagnostic& warning : warnings_) {
      warning.range = mapper(warning.range);
    }
  }

  /**
   * Record that warnings bound for this sink were discarded to enforce a resource limit elsewhere,
   * for example by a sink whose warnings were filtered before being forwarded here, so that
   * \ref resourceLimitExceeded reports the collection as incomplete. As after any resource limit,
   * later warnings are dropped. Does nothing when disabled.
   */
  void markResourceLimitExceeded() {
    if (enabled_) {
      resourceLimitExceeded_ = true;
    }
  }

  /// Append recorded diagnostics in order, preserving attempts even when the destination drops
  /// them.
  template <typename Read>
  void mergeRecorded(std::size_t count, std::size_t attempts, bool exceeded, Read&& read) {
    const auto total = warningAttempts_ + attempts;
    if (enabled_) {
      for (std::size_t i = 0; i < count && !resourceLimitExceeded_; ++i) {
        add([&] { return read(i); });
      }
      resourceLimitExceeded_ |= exceeded;
    }
    warningAttempts_ = total;
  }

  /// Merge all warnings from another collecting sink into this one.
  void merge(ParseWarningSink&& other) {
    mergeRecorded(other.warnings_.size(), other.warningAttempts_, other.resourceLimitExceeded_,
                  [&](std::size_t i) { return std::move(other.warnings_[i]); });
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
    const std::size_t mergedWarningAttempts = warningAttempts_ + other.warningAttempts_;
    if (enabled_) {
      for (auto& warning : other.warnings_) {
        warning.range.start = warning.range.start.addParentOffset(parentOffset);
        warning.range.end = warning.range.end.addParentOffset(parentOffset);
        add(std::move(warning));
      }
      resourceLimitExceeded_ |= other.resourceLimitExceeded_;
    }
    warningAttempts_ = mergedWarningAttempts;
  }

private:
  void tryAdd(ParseDiagnostic&& warning) {
    if (retainedCount() >= countLimit_ || warning.reason.size() > byteLimit_ - warningBytes_) {
      resourceLimitExceeded_ = true;
      return;
    }
    const auto bytes = warning.reason.size();
    if (consume_) {
      if (!consume_(context_, warning)) {
        resourceLimitExceeded_ = true;
        return;
      }
      ++delivered_;
    } else {
      warnings_.push_back(std::move(warning));
    }
    warningBytes_ += bytes;
  }

  std::vector<ParseDiagnostic> warnings_;
  void* context_ = nullptr;
  bool (*consume_)(void*, const ParseDiagnostic&) = nullptr;
  std::size_t delivered_ = 0;
  std::size_t countLimit_ = kMaximumWarnings;
  std::size_t byteLimit_ = kMaximumWarningBytes;
  std::size_t warningBytes_ = 0;
  bool enabled_ = true;
  bool resourceLimitExceeded_ = false;
  std::size_t warningAttempts_ = 0;
};

}  // namespace donner
