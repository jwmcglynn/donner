"""Darwin GPU variants of the tests selected by the native Metal lane."""

load("//build_defs:rules.bzl", "ci_platform_test")

def metal_ci_variants(names):
    """Declare each TestRunner in the package that owns its private executable."""
    for name in names:
        source = native.existing_rule(name)
        if source == None:
            fail("Missing Metal test: " + name)
        options = {key: source[key] for key in ["args", "size", "timeout", "flaky"] if key in source}
        if source.get("shard_count", -1) >= 0:
            options["shard_count"] = source["shard_count"]
        ci_platform_test(
            name = name + "_ci_metal",
            dep = ":" + name,
            darwin = True,
            metal_validation = True,
            target_platform = "//tools/ci:remote_macos_aarch64_gpu",
            exec_compatible_with = ["@platforms//os:macos"],
            tags = ["manual", "external", "no-cache", "no-local"],
            visibility = ["//tools/ci:__pkg__"],
            **options
        )

def metal_ci_suites(names):
    """Use the same membership for local and mixed-platform suites."""
    for name in names:
        tests = native.existing_rule(name)["tests"]
        native.test_suite(
            name = "unified_" + name,
            tags = ["manual"],
            tests = [str(test) + "_ci_metal" for test in tests],
        )
