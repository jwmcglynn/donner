"""
Helper rules, such as for building fuzzers.
"""

load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

def donner_cc_library(name, copts = [], tags = [], visibility = None, **kwargs):
    """
    Create a cc_library with donner-specific defaults.

    Args:
      name: Rule name.
      copts: List of copts.
      tags: List of tags.
      visibility: Visibility.
      **kwargs: Additional arguments, matching the implementation of cc_library.
    """

    package_path = native.package_name().split("/")
    if len(package_path) == 0:
        fail("Invalid package path: " + package_path)

    if package_path[0] != "donner" and package_path[0] != "experimental":
        fail("donner_cc_library can only be used in donner or experimental packages")

    # Tag experimental libraries
    if package_path[0] == "experimental":
        tags = tags + ["experimental"]

        # Disallow public visibility, require all paths be under //experimental
        for matcher in visibility:
            if not matcher.startswith("//experimental"):
                fail("Invalid visibility, must be under //experimental: " + matcher)

    cc_library(
        name = name,
        include_prefix = "/".join(package_path),
        copts = copts + ["-I."],
        tags = tags,
        visibility = visibility,
        **kwargs
    )

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
