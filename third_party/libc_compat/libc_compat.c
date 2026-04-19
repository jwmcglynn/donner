// Compatibility shim for Chromium's Debian Bullseye sysroot + hermetic LLVM
// toolchain (see //third_party/bazel/non_bcr_deps.bzl).
//
// The sysroot ships libc.so.6 with `copy_file_range@GLIBC_2.27` marked as a
// non-default version (single `@`, not `@@`), so unversioned references from
// toolchains_llvm's prebuilt libc++.a — specifically
// std::__fs::filesystem::detail::copy_file_impl in operations.cpp — don't
// auto-resolve at link time. Every other glibc symbol libc++ depends on is
// already default-versioned, so this is a one-symbol fix rather than a
// broader ABI mismatch.
//
// This shim defines `copy_file_range` as an ordinary symbol in our own code,
// pointed at the versioned glibc entry point via `.symver`. The linker picks
// up our definition, libc++ is happy, and the call forwards to glibc at
// runtime.

#if defined(__linux__) && !defined(__APPLE__)

#include <sys/types.h>

extern ssize_t donner_libc_compat_copy_file_range(int, off_t *, int, off_t *,
                                                  size_t, unsigned int);
__asm__(".symver donner_libc_compat_copy_file_range,copy_file_range@GLIBC_2.27");

ssize_t copy_file_range(int fd_in, off_t *off_in, int fd_out, off_t *off_out,
                        size_t len, unsigned int flags) {
  return donner_libc_compat_copy_file_range(fd_in, off_in, fd_out, off_out,
                                            len, flags);
}

#endif
