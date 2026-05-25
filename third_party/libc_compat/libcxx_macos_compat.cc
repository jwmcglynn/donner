// Compatibility shim for toolchains_llvm's LLVM 21 libc++ runtime on macOS.
//
// When Bazel links with the Xcode 26.5 SDK but runs on an older macOS host,
// libFuzzer/UBSan objects may reference std::__1::__hash_memory. That symbol
// exists in LLVM 21's libc++ but not in the host's /usr/lib/libc++.1.dylib,
// and the macOS C++ toolchain injects the system libc++ before rule linkopts
// can replace it. Provide the one missing ABI entry point in the executable.

#if defined(__APPLE__)

#include <cstddef>
#include <cstdint>

extern "C" std::size_t donner_libcxx_compat_hash_memory(const void* data,
                                                        std::size_t size)
    asm("__ZNSt3__113__hash_memoryEPKvm");

extern "C" std::size_t donner_libcxx_compat_hash_memory(const void* data,
                                                        std::size_t size) {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::uint64_t hash = 14695981039346656037ull;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ull;
  }

  if constexpr (sizeof(std::size_t) < sizeof(hash)) {
    hash ^= hash >> 32;
  }

  return static_cast<std::size_t>(hash);
}

#endif
