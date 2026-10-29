/// @file
/// Prints the identity of the C++ compiler that built this program, as one line.
///
/// The release CLI job builds this probe in the same Bazel invocation and configuration as the
/// released `donner-cli` binary, so both are compiled by the C++ toolchain Bazel resolved for that
/// build. `tools/release_cli.py` records this output as the CLI's compiler version.

#include <cstdio>

#if defined(__clang__)
// For example "Ubuntu Clang 18.1.3 (1ubuntu1)" or "Apple LLVM 17.0.0 (clang-1700.3.19.1)".
#define DONNER_COMPILER_IDENTITY __VERSION__
#elif defined(__GNUC__)
// GCC's __VERSION__ is only the version number, so name the compiler.
#define DONNER_COMPILER_IDENTITY "GCC " __VERSION__
#else
#error "release_compiler_identity supports only Clang and GCC"
#endif

int main() {
  return std::puts(DONNER_COMPILER_IDENTITY) < 0 ? 1 : 0;
}
