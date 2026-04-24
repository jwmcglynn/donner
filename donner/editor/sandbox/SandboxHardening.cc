#include "donner/editor/sandbox/SandboxHardening.h"

#include <sys/resource.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__linux__)
// Keep `<linux/bpf_common.h>` explicit — misc-include-cleaner can't
// always trace BPF_LD/BPF_W/BPF_ABS back through `<linux/filter.h>`.
#include <linux/audit.h>
#include <linux/bpf_common.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

#include <array>
#include <cstddef>
#endif

#if defined(__APPLE__)
#include <sandbox.h>
#endif

namespace donner::editor::sandbox {

namespace {

bool SetRlimit(int resource, rlim_t value, std::string& err) {
  rlimit r;
  r.rlim_cur = value;
  r.rlim_max = value;
  if (::setrlimit(resource, &r) != 0) {
    err = std::strerror(errno);
    return false;
  }
  return true;
}

/// Close all file descriptors strictly greater than FD 2 (stderr). Uses
/// the Linux-specific `close_range` syscall when available; falls back to
/// an explicit loop bounded by the current `RLIMIT_NOFILE` soft cap (or
/// a hard 4096 ceiling — whichever is smaller) because `sysconf(_SC_OPEN_MAX)`
/// returns `INT64_MAX` on macOS, which would take effectively forever to
/// sweep. Either path is best-effort — an already-closed FD is fine.
bool CloseFdsAboveStderr() {
#if defined(__linux__) && defined(SYS_close_range)
  // Close FDs [3, uint32_max) in one syscall. Returns 0 on success.
  const long rc = ::syscall(SYS_close_range, 3u, ~0u, 0u);
  if (rc == 0) return true;
  if (errno != ENOSYS) {
    // close_range is present but failed for some reason — fall through to
    // the manual sweep rather than reporting an error, since individual
    // close() calls are more forgiving.
  }
#endif

  // Pick an upper bound that's both safe (won't take forever) and
  // conservative (high enough to catch anything a parser child would
  // realistically have open). 4096 is well above the tens-of-FDs seen
  // in practice and still finishes instantly.
  constexpr long kHardCeiling = 4096;
  long maxFd = kHardCeiling;
  rlimit r;
  if (::getrlimit(RLIMIT_NOFILE, &r) == 0 && r.rlim_cur != RLIM_INFINITY) {
    const long softCur = static_cast<long>(r.rlim_cur);
    if (softCur > 0 && softCur < maxFd) maxFd = softCur;
  }
  for (long fd = 3; fd < maxFd; ++fd) {
    // Ignore EBADF — "not open" is the outcome we want anyway.
    (void)::close(static_cast<int>(fd));
  }
  return true;
}

// ---------------------------------------------------------------------------
// seccomp-bpf syscall allowlist (Linux only)
// ---------------------------------------------------------------------------
#if defined(__linux__)

// Determine the expected audit architecture at compile time.
#if defined(__aarch64__)
#define DONNER_SECCOMP_ARCH AUDIT_ARCH_AARCH64
#elif defined(__x86_64__)
#define DONNER_SECCOMP_ARCH AUDIT_ARCH_X86_64
#else
// Unsupported architecture — InstallSeccompFilter will be a no-op.
#define DONNER_SECCOMP_ARCH 0
#endif

/// Installs a seccomp-bpf filter that allows only the syscalls the parser
/// child needs. The default-deny action is chosen by the caller:
/// `SECCOMP_RET_KILL_PROCESS` (production) immediately terminates the
/// child on any unlisted syscall, and `SECCOMP_RET_ERRNO | EACCES`
/// (debug) lets the syscall return -EACCES so developers can extend
/// the allowlist without crashing the whole child.
///
/// Returns true on success, false if prctl/seccomp setup failed.
bool InstallSeccompFilter(HardeningOptions::SeccompDenyAction denyAction) {
#if DONNER_SECCOMP_ARCH == 0
  // Architecture not supported — silently skip.
  (void)denyAction;
  return true;
#else
  // PR_SET_NO_NEW_PRIVS is required before installing a seccomp filter
  // without CAP_SYS_ADMIN.
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    return false;
  }

  // The syscall allowlist. Each entry is a __NR_* constant. The BPF program
  // is generated from this table: for each entry, emit a JEQ that jumps to
  // ALLOW if the syscall number matches, otherwise falls through to the
  // next check. After all checks, the default action is SECCOMP_RET_ERRNO.
  //
  // Architecture-specific notes:
  //   - aarch64 has no __NR_fstat; glibc uses __NR_newfstatat instead.
  //   - x86_64 has both __NR_fstat and __NR_newfstatat.
  static constexpr int kAllowedSyscalls[] = {
      // I/O
      __NR_read,
      __NR_write,
      __NR_close,
#if defined(__NR_fstat)
      __NR_fstat,
#endif
#if defined(__NR_newfstatat)
      __NR_newfstatat,
#endif
      __NR_lseek,

      // Memory management
      __NR_brk,
      __NR_mmap,
      __NR_munmap,
      __NR_mremap,
      __NR_mprotect,
      __NR_madvise,

      // Threading / signals
      __NR_futex,
      __NR_rt_sigaction,
      __NR_rt_sigprocmask,
      __NR_rt_sigreturn,

      // Process exit
      __NR_exit,
      __NR_exit_group,

      // Information
      __NR_getpid,
      __NR_gettid,
      __NR_clock_gettime,
      __NR_getrandom,

      // Resource limits (needed for prlimit64 self-query)
      __NR_prlimit64,

      // glibc startup / TLS
      __NR_set_tid_address,
      __NR_set_robust_list,
      __NR_rseq,

  // x86_64-specific: arch_prctl is used by glibc for TLS/FS base setup.
  // Not present on aarch64.
#if defined(__NR_arch_prctl)
      __NR_arch_prctl,
#endif
  // x86_64 glibc may use access/openat/stat during CRT init (locale,
  // NSS) even after our FD sweep. Allow them so the child doesn't get
  // stuck during early init on CI runners.
#if defined(__NR_access)
      __NR_access,
#endif
#if defined(__NR_openat)
      __NR_openat,
#endif
#if defined(__NR_stat)
      __NR_stat,
#endif
  // poll/ppoll needed by some glibc I/O paths (e.g. stdio locking).
#if defined(__NR_poll)
      __NR_poll,
#endif
#if defined(__NR_ppoll)
      __NR_ppoll,
#endif
  // ioctl(TIOCGWINSZ) may be called by glibc on stderr.
#if defined(__NR_ioctl)
      __NR_ioctl,
#endif
  // sched_getaffinity used by some allocators for arena sizing.
#if defined(__NR_sched_getaffinity)
      __NR_sched_getaffinity,
#endif
  };

  static constexpr size_t kNumAllowed = sizeof(kAllowedSyscalls) / sizeof(kAllowedSyscalls[0]);

  // BPF program layout:
  //   [0]   Load arch from seccomp_data
  //   [1]   JEQ arch -> [3], else [2]
  //   [2]   KILL_PROCESS (wrong architecture)
  //   [3]   Load syscall number from seccomp_data
  //   [4..4+2*N-1]  For each allowed syscall:
  //           JEQ __NR_xxx -> ALLOW (next instruction)
  //           RET ALLOW
  //         (the JEQ skips 0 instructions on match to hit the RET ALLOW
  //          immediately following, or skips 1 to fall through to the next
  //          JEQ pair)
  //   [4+2*N]  Default: RET ERRNO(EACCES)
  //
  // Total instructions: 4 + 2*N + 1 = 5 + 2*N

  static constexpr size_t kFilterLen = 5 + 2 * kNumAllowed;

  // Build the filter program. We use a std::array so the size is known at
  // compile time and we avoid VLA or dynamic allocation.
  std::array<struct sock_filter, kFilterLen> filter{};
  size_t idx = 0;

  // [0] Load architecture field from seccomp_data.
  filter[idx++] = BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch));

  // [1] Check architecture matches expected. Skip next instruction if match.
  filter[idx++] = BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, DONNER_SECCOMP_ARCH, 1, 0);

  // [2] Architecture mismatch: kill the process.
  filter[idx++] = BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);

  // [3] Load syscall number from seccomp_data.
  filter[idx++] = BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr));

  // [4..4+2*N-1] For each allowed syscall, emit a JEQ + RET ALLOW pair.
  for (size_t i = 0; i < kNumAllowed; ++i) {
    // JEQ: if syscall == kAllowedSyscalls[i], jump to next instruction
    // (jt=0 means skip 0, landing on the RET ALLOW); otherwise skip 1
    // to jump over the RET ALLOW to the next JEQ pair.
    filter[idx++] =
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<__u32>(kAllowedSyscalls[i]), 0, 1);
    filter[idx++] = BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
  }

  // [4+2*N] Default deny. `kKillProcess` is the production action —
  // an unlisted syscall gets SIGSYS immediately. `kErrno` is available
  // for ad-hoc allowlist exploration; it returns -EACCES so the
  // program can continue and hit the next missing entry. Don't ship
  // with `kErrno`.
  // Cast target is `uint32_t` rather than `__u32` so misc-include-cleaner
  // doesn't need to trace the Linux typedef to `<linux/types.h>`; the
  // layout is identical in practice.
  const uint32_t denyReturn = (denyAction == HardeningOptions::SeccompDenyAction::kErrno)
                                  ? (SECCOMP_RET_ERRNO | (EACCES & SECCOMP_RET_DATA))
                                  : SECCOMP_RET_KILL_PROCESS;
  filter[idx++] = BPF_STMT(BPF_RET | BPF_K, denyReturn);

  struct sock_fprog prog {};
  prog.len = static_cast<unsigned short>(idx);
  prog.filter = filter.data();

  return ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) == 0;
#endif  // DONNER_SECCOMP_ARCH != 0
}

#endif  // defined(__linux__)

// ---------------------------------------------------------------------------
// macOS `sandbox_init` profile (Apple only)
// ---------------------------------------------------------------------------
#if defined(__APPLE__)

/// Result of an attempt to install the macOS sandbox profile.
enum class SandboxInstallResult {
  kApplied,           ///< `sandbox_init` returned 0 — profile is active.
  kAlreadySandboxed,  ///< EPERM — the caller's parent already sandboxed us.
  kFailed,            ///< Hard failure (bad profile, kernel refusal, etc.).
};

/// Installs the `kSBXProfileNoWriteExceptTemporary` profile via the
/// legacy `sandbox_init` API. This is the macOS analogue of the Linux
/// seccomp allowlist: the child keeps its already-open stdin/stdout/
/// stderr pipes, may read/compute freely, but new filesystem writes
/// outside of temp/cache are denied.
///
/// `kSBXProfilePureComputation` is strictly tighter and is what we'd
/// pick in the abstract, but it blocks the mach-service lookups the
/// Geode renderer's wgpu-native → Metal pipeline makes during live
/// rendering (SIGSEGV inside `libMetal` on first pipeline compile).
/// `kSBXProfileNoWriteExceptTemporary` keeps Metal functional while
/// still closing the door on arbitrary disk writes from a parser bug.
///
/// Returns `kAlreadySandboxed` (EPERM) when the parent process already
/// sandboxed us — the primary path that hits this is Bazel's macOS
/// darwin-sandbox during tests. Callers should treat that as "hardening
/// satisfied by the outer sandbox" and not a fatal error. Hard failures
/// (bad profile, kernel refusal) are reported via `kFailed` + `errOut`.
///
/// The API is deprecated since macOS 10.8 but still supported (the OS
/// itself uses it for system services). We suppress the deprecation
/// warning locally because the successor SPI (`sandbox_init_with_parameters`
/// + custom SBPL profiles) is private and not appropriate for a portable
/// parser child.
SandboxInstallResult InstallMacOSSandboxProfile(std::string& errOut) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  char* errorBuf = nullptr;
  errno = 0;
  const int rc = ::sandbox_init(kSBXProfileNoWriteExceptTemporary, SANDBOX_NAMED, &errorBuf);
  if (rc == 0) {
    if (errorBuf != nullptr) ::sandbox_free_error(errorBuf);
    return SandboxInstallResult::kApplied;
  }
  const int savedErrno = errno;
  std::string err;
  if (errorBuf != nullptr) {
    err = errorBuf;
    ::sandbox_free_error(errorBuf);
  } else if (savedErrno != 0) {
    err = std::strerror(savedErrno);
  } else {
    err = "sandbox_init returned -1 (no error buffer)";
  }
#pragma clang diagnostic pop
  if (savedErrno == EPERM) {
    // Already inside a sandbox (bazel darwin-sandbox, SIP-tight shells,
    // or an enclosing profile) — our `kSBXProfileNoWriteExceptTemporary` request
    // is strictly tighter than any realistic outer profile, so we treat
    // this as "hardening already in force" rather than a fatal error.
    errOut = err;
    return SandboxInstallResult::kAlreadySandboxed;
  }
  errOut = err;
  return SandboxInstallResult::kFailed;
}

#endif  // defined(__APPLE__)

void LogSummary(const HardeningOptions& options) {
  // Keep this single-line and stable — tests grep for the "sandbox
  // hardening:" prefix. Anything on stderr is captured by the host as
  // diagnostics and surfaced through `RenderResult::diagnostics`.
  const char* denyAction =
      (options.seccompDenyAction == HardeningOptions::SeccompDenyAction::kKillProcess) ? "kill"
                                                                                       : "errno";
  std::fprintf(stderr,
               "sandbox hardening: as=%zu cpu=%u fsize=%zu nofile=%u chdir=%d fdsweep=%d "
               "seccomp=%d seccomp_deny=%s sbxprof=%d\n",
               options.addressSpaceBytes, options.cpuSeconds, options.maxFileBytes,
               options.maxOpenFiles, options.chdirRoot ? 1 : 0, options.closeExtraFds ? 1 : 0,
               options.installSeccompFilter ? 1 : 0, denyAction,
               options.installSandboxProfile ? 1 : 0);
}

}  // namespace

HardeningResult ApplyHardening(const HardeningOptions& options) {
  HardeningResult result;

  if (options.requireSandboxEnv) {
    const char* marker = std::getenv("DONNER_SANDBOX");
    if (marker == nullptr || std::strcmp(marker, "1") != 0) {
      result.status = HardeningStatus::kMissingSandboxEnv;
      result.message =
          "refusing to run without DONNER_SANDBOX=1 — this binary must be "
          "launched via SandboxHost, not executed directly";
      return result;
    }
  }

  if (options.chdirRoot) {
    if (::chdir("/") != 0) {
      result.status = HardeningStatus::kChdirFailed;
      result.message = std::string("chdir(\"/\"): ") + std::strerror(errno);
      return result;
    }
  }

  // Close inherited FDs BEFORE installing RLIMIT_NOFILE — the NOFILE cap
  // lowers the ceiling on future opens but doesn't retroactively close
  // already-open descriptors.
  if (options.closeExtraFds) {
    if (!CloseFdsAboveStderr()) {
      result.status = HardeningStatus::kCloseFdsFailed;
      result.message = "FD sweep failed";
      return result;
    }
  }

  std::string errMessage;
  if (options.addressSpaceBytes > 0) {
    // macOS exposes `RLIMIT_AS` as an alias for `RLIMIT_RSS`, and the
    // kernel rejects user-mode attempts to lower it with `EINVAL`. We
    // leave the cap unset there and rely on the sandbox profile (when
    // wired) + RLIMIT_DATA to bound memory growth. See
    // docs/design_docs/0023-editor_sandbox.md §S6.2 for the hardening
    // matrix per platform.
#if !defined(__APPLE__)
    if (!SetRlimit(RLIMIT_AS, static_cast<rlim_t>(options.addressSpaceBytes), errMessage)) {
      result.status = HardeningStatus::kResourceLimitFailed;
      result.message = "RLIMIT_AS: " + errMessage;
      return result;
    }
#else
    // Best-effort data-segment cap on macOS — softer than AS but still
    // bounds runaway `brk()` / `sbrk()` growth from a parser bug.
    if (!SetRlimit(RLIMIT_DATA, static_cast<rlim_t>(options.addressSpaceBytes), errMessage)) {
      // Don't fail startup if DATA can't be set either — the sandbox
      // profile is the primary backstop on macOS.
      result.message = "RLIMIT_DATA (non-fatal on macOS): " + errMessage;
    }
#endif
  }
  if (options.cpuSeconds > 0) {
    if (!SetRlimit(RLIMIT_CPU, static_cast<rlim_t>(options.cpuSeconds), errMessage)) {
      result.status = HardeningStatus::kResourceLimitFailed;
      result.message = "RLIMIT_CPU: " + errMessage;
      return result;
    }
  }
  // RLIMIT_FSIZE on macOS interacts badly with the Geode renderer:
  // wgpu-native's Metal backend writes shader pipeline caches to
  // `~/Library/Caches`, and a 0-byte FSIZE cap triggers SIGXFSZ on the
  // first render. The macOS `sandbox_init` profile already constrains
  // new filesystem writes (allowing only inherited FDs + temp/cache
  // paths), so the rlimit is redundant there. On Linux it stays as
  // the belt-and-suspenders check alongside seccomp.
#if !defined(__APPLE__)
  if (!SetRlimit(RLIMIT_FSIZE, static_cast<rlim_t>(options.maxFileBytes), errMessage)) {
    result.status = HardeningStatus::kResourceLimitFailed;
    result.message = "RLIMIT_FSIZE: " + errMessage;
    return result;
  }
#endif
  if (options.maxOpenFiles > 0) {
    if (!SetRlimit(RLIMIT_NOFILE, static_cast<rlim_t>(options.maxOpenFiles), errMessage)) {
      result.status = HardeningStatus::kResourceLimitFailed;
      result.message = "RLIMIT_NOFILE: " + errMessage;
      return result;
    }
  }

  // Install the platform syscall/profile filter LAST. All preceding
  // setup steps (chdir, FD sweep, rlimits) may need operations that the
  // filter would deny, so it must not be active until setup is complete.
#if defined(__linux__)
  if (options.installSeccompFilter) {
    if (!InstallSeccompFilter(options.seccompDenyAction)) {
      result.status = HardeningStatus::kSeccompFailed;
      result.message = std::string("seccomp-bpf installation failed: ") + std::strerror(errno);
      return result;
    }
  }
#endif

#if defined(__APPLE__)
  if (options.installSandboxProfile) {
    std::string sandboxErr;
    switch (InstallMacOSSandboxProfile(sandboxErr)) {
      case SandboxInstallResult::kApplied: break;
      case SandboxInstallResult::kAlreadySandboxed:
        // Outer sandbox (bazel darwin-sandbox, etc.) is already in force.
        // Log a single line so reviewers can see the state without the
        // test failing on nested-sandbox EPERM.
        std::fprintf(stderr,
                     "sandbox hardening: macOS profile skipped (already "
                     "sandboxed by parent: %s)\n",
                     sandboxErr.c_str());
        break;
      case SandboxInstallResult::kFailed:
        result.status = HardeningStatus::kSandboxProfileFailed;
        result.message = "sandbox_init failed: " + sandboxErr;
        return result;
    }
  }
#endif

  if (options.logSummaryToStderr) {
    LogSummary(options);
  }

  return result;
}

}  // namespace donner::editor::sandbox
