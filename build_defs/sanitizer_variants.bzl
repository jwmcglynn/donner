"""The flags of each sanitizer variant, shared by its transition and checked against .bazelrc.

A sanitizer variant of a test builds the test and everything it links with the compile and link
options of the matching `.bazelrc` config (`--config=asan` or `--config=tsan`), minus the toolchain
selection: the variant inherits the toolchain of the invocation, which in CI is already the
hermetic LLVM toolchain. //build_defs:sanitizer_variants_tests checks every flag of those configs
against these lists, so a change to one must change the other, and checks the test environment
against its list of deliberate differences. A variant runs only in an invocation without a
sanitizer config of its own (//build_defs:sanitizer); elsewhere it is incompatible and skipped.
"""

SANITIZER_VARIANTS = {
    "asan": struct(
        features = ["asan"],
        copts = [
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-fsanitize-address-use-after-scope",
            "-fno-sanitize=vptr,function",
        ],
        linkopts = [
            "-ldl",
            "-fsanitize=address,undefined",
            "-fno-sanitize=vptr,function",
        ],
        # Leak detection is off: LeakSanitizer stops the process with ptrace at exit, which the
        # self-hosted lane's remote test sandbox blocks, so every ASan variant would hang there.
        # The hosted lane shares this definition, so leak detection is off on both. Memory-safety
        # checks are unaffected, and the nightly ASan lane still checks leaks.
        test_env = {"ASAN_OPTIONS": "detect_leaks=0"},
    ),
    "tsan": struct(
        features = [],
        copts = [
            "-fsanitize=thread",
            "-fno-omit-frame-pointer",
        ],
        linkopts = [
            "-ldl",
            "-fsanitize=thread",
        ],
        test_env = {"TSAN_OPTIONS": "halt_on_error=1:second_deadlock_stack=1"},
    ),
}

# Native CI emits sanitizer variants after the package's own rules. A None test list includes
# every eligible test in that package. Manual and fuzzer corpus tests have separate scheduling.
SANITIZER_VARIANT_PACKAGES = {
    "donner/base/tasks": struct(
        sanitizers = ("asan", "tsan"),
        test_names = None,
    ),
    "donner/svg/components/style": struct(
        sanitizers = ("tsan",),
        test_names = ("style_parallel_tests",),
    ),
}
