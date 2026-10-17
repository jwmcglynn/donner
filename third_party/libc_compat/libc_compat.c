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
// Call the kernel directly: a versioned glibc reference can bind back to this
// interposed symbol when the shim is a shared library, causing infinite recursion.

#if defined(__linux__) && !defined(__APPLE__)

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

_Static_assert(sizeof(off_t) == sizeof(int64_t), "copy_file_range requires 64-bit offsets");

// Kernel errors are preserved; this shim does not add pthread cancellation handling.
ssize_t copy_file_range(int fd_in, off_t *off_in, int fd_out, off_t *off_out,
                        size_t len, unsigned int flags) {
  return syscall(SYS_copy_file_range, fd_in, off_in, fd_out, off_out, len, flags);
}

#endif
