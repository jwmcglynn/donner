#pragma once
/// @file

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "donner/css/ComponentValue.h"

namespace donner::css {

/// Classification of variable references, including references nested in functions and blocks.
enum class VariableReferences { None, Valid, Invalid };

/// Inspect the token tree without substituting or looking up any name.
VariableReferences InspectVariableReferences(std::span<const ComponentValue> values);

/// Visit every referenced custom property, including those in unused fallbacks, in token order.
/// Returns false when a var() argument list is invalid.
bool VisitVariableReferences(std::span<const ComponentValue> values,
                             const std::function<void(std::string_view)>& visit);

/// A lookup returns an already computed token sequence. Its storage must outlive substitution.
/// An engaged empty span is a valid empty value; a missing name is guaranteed-invalid.
using VariableLookup =
    std::function<std::optional<std::span<const ComponentValue>>(std::string_view)>;

/// Per-value limits in addition to any enclosing cascade work limits.
struct VariableSubstitutionLimits {
  std::size_t components = 65'536;
  std::size_t bytes = 1024 * 1024;
};

/// A rejected expansion has no partially substituted output.
enum class VariableSubstitutionStatus { Resolved, Invalid, LimitExceeded };
struct VariableSubstitutionResult {
  VariableSubstitutionStatus status;
  std::vector<ComponentValue> values;
};

/// Substitute already computed variables and selected fallback token sequences. Token boundaries,
/// nesting, and source locations are preserved. The optional callback admits actual output work
/// (component count, source bytes) before each append, and may decline further work.
VariableSubstitutionResult SubstituteVariables(
    std::span<const ComponentValue> values, const VariableLookup& lookup,
    VariableSubstitutionLimits limits = {},
    const std::function<bool(std::size_t, std::size_t)>& admit = {});

}  // namespace donner::css
