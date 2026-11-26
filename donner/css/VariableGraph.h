#pragma once
/// @file

#include "donner/css/VariableSubstitution.h"

namespace donner::css {

/// One winning local custom declaration. A missing value represents the initial,
/// guaranteed-invalid value and masks an inherited value of the same name. Declarations that
/// inherit unchanged can be omitted; the lookup supplies their already computed parent value.
struct VariableDefinition {
  std::string_view name;
  std::optional<std::span<const ComponentValue>> value;
};

/// The caller owns the canonical computed sequences. Publishing one result before evaluating
/// its dependents permits retained-byte admission at allocation time, without keeping a second
/// uncharged copy of every expanded value.
struct VariableOutputs {
  /// Read a previously published local result by its definition index.
  std::function<std::optional<std::span<const ComponentValue>>(std::size_t)> lookup;
  /// Store one result by definition index. Return false on refusal.
  std::function<bool(std::size_t, VariableSubstitutionResult)> publish;
};

/// Compute local values in selected dependency order. Each name occurs once, and all definition
/// spans must stay alive throughout the call. The caller keeps already published and inherited
/// values stable while an individual value is computed. Cycles invalidate exactly their
/// participants; unused fallbacks create no dependency, while a selected fallback can. A
/// dependent can use a fallback after a referenced cycle becomes guaranteed-invalid.
///
/// Returns false on an invalid definition list, bounded graph-work exhaustion, aggregate admission
/// refusal, or output refusal. No result is published after a refusal; the caller discards any
/// earlier staged results. An invalid individual value, including one that exceeds a per-value
/// limit, is published as such.
bool ResolveVariableDefinitions(std::span<const VariableDefinition> local,
                                const VariableLookup& inherited, const VariableOutputs& outputs,
                                VariableSubstitutionLimits limits = {},
                                const std::function<bool(std::size_t, std::size_t)>& admit = {});

}  // namespace donner::css
