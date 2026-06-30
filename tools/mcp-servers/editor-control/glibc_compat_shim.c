// Toolchain/sysroot link shim — see BUILD.bazel `glibc_compat_shim`.
//
// The hermetic `toolchains_llvm` sysroot ships a `libc.so.6` that exports
// `copy_file_range` only as a non-default versioned symbol
// (`copy_file_range@GLIBC_2.27`). LLVM's static `libc++.a` bundles
// `std::filesystem::detail::copy_file_impl` (in `operations.cpp.o`) alongside
// the filesystem helpers this target actually uses (`current_path`, `remove`,
// `temp_directory_path`, ...), so that whole object is pulled into the link
// and its *unversioned* reference to `copy_file_range` cannot bind against the
// versioned-only sysroot symbol — `ld.lld` fails with "undefined symbol:
// copy_file_range". (No source here calls `std::filesystem::copy_file`.)
//
// Provide a strong, unversioned definition that forwards to the raw syscall so
// the reference resolves. `copy_file_range(2)` exists on Linux >= 4.5, which
// every host that runs these binaries satisfies. Remove this shim once the
// toolchain sysroot is bumped to a glibc that exports `copy_file_range` as the
// default symbol version.
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

ssize_t copy_file_range(int fd_in, loff_t* off_in, int fd_out, loff_t* off_out, size_t len,
                        unsigned int flags) {
  return syscall(__NR_copy_file_range, fd_in, off_in, fd_out, off_out, len, flags);
}
