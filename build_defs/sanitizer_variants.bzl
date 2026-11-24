"""The flags of each sanitizer variant, shared by its transition and checked against .bazelrc.

A sanitizer variant of a test builds the test and everything it links with the compile and link
options of the matching `.bazelrc` config (`--config=asan` or `--config=tsan`), minus the toolchain
selection: the variant inherits the toolchain of the invocation, which in CI is already the
hermetic LLVM toolchain. //build_defs:sanitizer_variants_tests checks the compile and link lists
against `.bazelrc`, so a change to one config must change the other. The test environment is the
variant's own.
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
        # remote test sandbox of the native CI invocation blocks, so every ASan variant would hang
        # there. Memory-safety checks are unaffected, and the nightly ASan lane still checks leaks.
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

# Packages whose every test gets sanitizer variants, and which sanitizers. The native CI package
# finalizer emits them after the package's own rules, so a test added anywhere in a listed package
# gets its variants. Today only the task scheduler, whose races and lifetime faults are often
# visible only to a sanitizer.
SANITIZER_VARIANT_PACKAGES = {
    "donner/base/tasks": ("asan", "tsan"),
}
