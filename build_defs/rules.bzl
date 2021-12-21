"""
Helper rules, such as for building fuzzers.
"""

load("@rules_cc//cc:defs.bzl", "cc_test")

def donner_cc_fuzzer(name, corpus, **kwargs):
    """
    Create a libfuzzer-based fuzz target.

    Args:
      name: Rule name.
      corpus: Path to a corpus directory, or a filegroup rule for the corpus.
      **kwargs: Additional arguments, matching the implementation of cc_test.
    """
    if not (corpus.startswith("//") or corpus.startswith(":")):
        corpus_name = name + "_corpus"
        corpus = native.glob([corpus + "/**"])
        native.filegroup(name = corpus_name, srcs = corpus)
    else:
        corpus_name = corpus

    cc_test(
        name = name,
        linkopts = ["-fsanitize=fuzzer"],
        args = ["$(locations %s)" % corpus_name],
        linkstatic = 1,
        data = [corpus_name],
        # Only run on Linux, since the macOS clang is missing libclang_rt.fuzzer_osx.a.
        target_compatible_with = [
            "@platforms//os:linux",
        ],
        **kwargs
    )
