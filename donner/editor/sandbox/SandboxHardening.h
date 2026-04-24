#pragma once
/// @file
///
/// Defense-in-depth hardening applied inside the sandbox child process
/// immediately after startup, before it reads any untrusted input. See
/// docs/design_docs/0023-editor_sandbox.md §"Milestone S6".
///
/// S6.1 (this file) ships the portable hardening that works on Linux and
/// macOS without extra dependencies:
///
///   - **Environment gate**: the child refuses to run unless
///     `DONNER_SANDBOX=1` is set, so accidental direct execution (e.g., a
///     developer running the binary by hand to reproduce a crash) fails
///     loudly rather than silently behaving like a sandboxed child.
///   - **`chdir("/")`**: strips the child's relative-path authority so a
///     bug that tries to open "foo.txt" can't hit a file the host was in
///     the middle of editing.
///   - **FD sweep**: closes every inherited file descriptor above stderr.
///     Prevents the child from accidentally holding a writable FD to the
///     host's terminal, log file, or runfiles tree.
///   - **`setrlimit` caps**: `RLIMIT_AS` (address space), `RLIMIT_CPU`
///     (wall CPU seconds), `RLIMIT_FSIZE` (regular-file writes), and
///     `RLIMIT_NOFILE` (max open files). A parser OOM or infinite-loop
///     pathological SVG is bounded at the kernel level, not relying on
///     host-side watchdogs.
///
/// S6.2 (follow-up): `seccomp-bpf` syscall allowlist on Linux,
/// `sandbox_init` deny-all profile on macOS, and optional per-UID
/// isolation. Those are intentionally out of scope for this milestone —
/// they carry real risk of breaking the child in architecture-specific
/// ways, and landing them behind unit tests needs more than a session's
/// worth of iteration.

#include <cstddef>
#include <cstdint>
#include <string>

namespace donner::editor::sandbox {

/// Knobs for which hardening measures to apply. Defaults match the "strict
/// production sandbox child" profile the design doc calls out. Tests and
/// developer tools can relax individual measures — e.g., setting
/// `requireSandboxEnv = false` makes the child runnable by hand under a
/// debugger.
struct HardeningOptions {
  /// Verify that `DONNER_SANDBOX=1` is set in the environment. The child
  /// refuses to run otherwise. Set to `false` only for ad-hoc developer
  /// debugging — the `SandboxHost` always sets this in production.
  bool requireSandboxEnv = true;

  /// `RLIMIT_AS` cap in bytes. 0 means "leave unset". Default 1 GiB —
  /// large enough for any real SVG the parser might touch, small enough
  /// that a runaway allocation is caught at the kernel boundary.
  std::size_t addressSpaceBytes = 1024uLL * 1024uLL * 1024uLL;

  /// `RLIMIT_CPU` cap in wall seconds. 0 = unset. Default 30 s.
  unsigned int cpuSeconds = 30;

  /// `RLIMIT_FSIZE` cap in bytes. 0 means "no file writes allowed" — pipe
  /// writes (stdout) are unaffected because pipes are not regular files.
  std::size_t maxFileBytes = 0;

  /// `RLIMIT_NOFILE` cap. Default 16 (stdin/stdout/stderr plus headroom
  /// for any internal FDs glibc may open transiently).
  unsigned int maxOpenFiles = 16;

  /// Change working directory to `/` so relative paths can't escape.
  bool chdirRoot = true;

  /// Close all inherited file descriptors above stderr (FD 2) before
  /// reading any untrusted input.
  bool closeExtraFds = true;

  /// Emit a single-line summary of the applied profile to stderr. Useful
  /// for tests (which grep for the marker) and for debugging.
  bool logSummaryToStderr = true;

  /// Install a seccomp-bpf syscall allowlist (Linux only). Denied syscalls
  /// are handled according to `seccompDenyAction` (below). On non-Linux
  /// platforms this field is silently ignored.
  bool installSeccompFilter = true;

  /// How a denied syscall is handled when the seccomp-bpf filter is
  /// active. `kKillProcess` is the production default — an unexpected
  /// syscall kills the child immediately with `SIGSYS`, making it
  /// impossible for a parser bug to silently succeed under the
  /// denial. `kErrno` returns `-EACCES` instead, which is softer but
  /// permits programs to recover; use it only for ad-hoc debugging
  /// when fleshing out the allowlist. Ignored on non-Linux.
  enum class SeccompDenyAction {
    kKillProcess,  ///< SECCOMP_RET_KILL_PROCESS — SIGSYS terminates the child.
    kErrno,        ///< SECCOMP_RET_ERRNO | EACCES — syscall fails with -EACCES.
  };
  SeccompDenyAction seccompDenyAction = SeccompDenyAction::kKillProcess;

  /// Install a macOS `sandbox_init` profile (`kSBXProfilePureComputation`)
  /// as the analogous defense-in-depth layer on macOS. Denies filesystem
  /// writes, new network connections, mach lookups, etc. — the child
  /// keeps its inherited stdin/stdout/stderr pipes and heap. On non-macOS
  /// platforms this field is silently ignored.
  bool installSandboxProfile = true;
};

/// Classifies the outcome of `ApplyHardening`. On any non-kOk status the
/// child should exit with a usage error — hardening is all-or-nothing.
enum class HardeningStatus {
  kOk,                    ///< Every requested measure took effect.
  kMissingSandboxEnv,     ///< DONNER_SANDBOX was not set to 1.
  kChdirFailed,           ///< chdir("/") returned non-zero.
  kCloseFdsFailed,        ///< FD sweep failed in a way we can't recover from.
  kResourceLimitFailed,   ///< One of the setrlimit calls failed.
  kSeccompFailed,         ///< seccomp-bpf filter installation failed.
  kSandboxProfileFailed,  ///< macOS `sandbox_init` returned non-zero.
};

struct HardeningResult {
  HardeningStatus status = HardeningStatus::kOk;
  /// Single-line diagnostic, suitable for stderr. Empty on success.
  std::string message;
};

/// Applies the requested hardening measures in order. Intended to be
/// called exactly once at the start of the child's `main()`, after argv
/// parsing but before reading stdin. Safe to call from multi-threaded
/// code in theory, but the child is single-threaded by design and the
/// caller is expected to run this before spawning any workers.
HardeningResult ApplyHardening(const HardeningOptions& options = {});

}  // namespace donner::editor::sandbox
