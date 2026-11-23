"""Clang `-fconstexpr-steps` budgets for targets that compile WGSL during C++ compilation.

Clang applies the limit to each constant evaluation separately. In these targets the largest
evaluation parses or emits one WGSL program with `wgsl::Compile`, so a budget follows the size of
its program, not the number of shaders in the file. A test source that chains several mutations
of a large program into one initializer multiplies that cost, so the image blit, Slug fill and
specular lighting test sources make one replacement per constant.

The standard library's bounds checks count as evaluation steps. libstdc++ 15 checks `std::array`
and `std::string_view` subscripts during constant evaluation and enables `_GLIBCXX_ASSERTIONS`
in builds without optimization, so host clang with libstdc++ 15 and no `-O` (the GitHub-hosted
Linux build) needs the most steps of the supported toolchains. Each budget is at least 1.5 times
the largest step count measured there among its targets. The counts below are for clang 21 with
libstdc++ 15, then with libc++ 21.

Clang reports only a failure, never a count. To re-derive a count after a program grows, compile
each source with that toolchain and no `-O` while bisecting `-fconstexpr-steps=N` between a
failing and a passing value. Targets without an entry here use Clang's default of 1,048,576; each
of them compiles there within 1,048,576 / 1.5 steps.
"""

# Image blit: parsing the 25 KB program takes 1.84M to 1.89M steps (libc++: 1.47M to 1.68M).
IMAGE_BLIT_CONSTEXPR_COPTS = ["-fconstexpr-steps=4194304"]

# Slug mask: 2.52M to 2.62M steps (libc++: 1.68M to 2.10M).
SLUG_MASK_CONSTEXPR_COPTS = ["-fconstexpr-steps=4194304"]

# Slug fill, the ray-event program: 3.78M to 4.19M steps (libc++: 3.36M to 4.19M).
SLUG_FILL_CONSTEXPR_COPTS = ["-fconstexpr-steps=8388608"]

# Slug gradient: 3.36M to 3.57M steps (libc++: 2.52M to 2.94M).
SLUG_GRADIENT_CONSTEXPR_COPTS = ["-fconstexpr-steps=8388608"]

# Gaussian blur emitted to every projection at once (its all-projection probe, test artifact and
# compiler tests): 0.68M to 0.73M steps (libc++: 0.52M to 0.63M).
GAUSSIAN_BLUR_ALL_PROJECTIONS_CONSTEXPR_COPTS = ["-fconstexpr-steps=2097152"]
