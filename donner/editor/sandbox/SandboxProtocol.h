#pragma once
/// @file
///
/// Shared protocol constants between the editor host and the `donner_parser_child`
/// sandbox binary. See docs/design_docs/0023-editor_sandbox.md Milestone S1 for the
/// one-shot byte-in / PNG-out contract these exit codes implement.

namespace donner::editor::sandbox {

/// Child exit codes. The host classifies these into `SandboxStatus` values.
/// Values follow BSD `sysexits.h` convention where plausible.
inline constexpr int kExitOk = 0;
inline constexpr int kExitUsageError = 64;
inline constexpr int kExitParseError = 65;
inline constexpr int kExitRenderError = 66;

}  // namespace donner::editor::sandbox
