"""Expose existing build and test roots in the Darwin target configuration."""

load("//build_defs:rules.bzl", "ci_platform_build", "ci_platform_test")

def _test_options(source):
    options = {key: source[key] for key in ["args", "data", "size", "timeout", "flaky"] if key in source}
    if source.get("shard_count", -1) >= 0:
        options["shard_count"] = source["shard_count"]
    return options

def _runs_in_native_lane(tags):
    if any([tag in tags for tag in ["manual", "perf", "ci-remote-gpu", "fuzz_soak"]]):
        return False
    if "requires_desktop_session" in tags:
        return "requires_metal_toolchain" not in tags
    if "launches-browser" in tags:
        return True
    return not any([tag in tags for tag in ["requires_shader_validation", "requires_metal_toolchain"]])

def _variant(name, source, profile, run, gpu, prefix, extra_tags):
    tags = list(source.get("tags", []))
    attrs = {
        "name": prefix + name,
        "dep": ":" + name,
        "darwin": True,
        "darwin_profile": profile,
        "target_platform": "//tools/ci:remote_macos_aarch64_gpu" if gpu else "//tools/ci:remote_macos_aarch64",
        "target_compatible_with": source.get("target_compatible_with", []),
        "tags": tags + ["manual", "ci-darwin-test" if run else "ci-darwin-build"] + extra_tags,
        "testonly": True,
        "visibility": ["//tools/ci:__pkg__"],
    }
    if run:
        ci_platform_test(
            exec_compatible_with = ["@platforms//os:macos"],
            exec_properties = source.get("exec_properties", {}),
            **(attrs | _test_options(source))
        )
    else:
        ci_platform_build(**attrs)

def ci_darwin_targets(geode = [], diagnostics = [], qualification = [], fresh = []):
    """Declare manual variants at package end, where private dependencies are visible."""
    if native.repository_name() not in ["@", "@@"]:
        return
    for name, source in native.existing_rules().items():
        tags = list(source.get("tags", []))
        if name in geode:
            _variant(name, source, "geode", True, True, "ci_darwin_geode_", [])
        if name in qualification:
            _variant(name, source, "ordinary", True, True, "ci_darwin_qualify_", ["external", "no-cache"])
        if name in diagnostics:
            _variant(name, source, "ordinary", True, True, "ci_darwin_diagnostic_", ["external", "no-cache"])
        if "manual" in tags or source["kind"] in ["test_suite", "package_group", "toolchain_type"]:
            continue
        run = "shard_count" in source and _runs_in_native_lane(tags)
        gpu = "resources:local_gpu:1" in tags or "launches-browser" in tags
        profile = "desktop" if "requires_desktop_session" in tags else "ordinary"
        # Filtered tests still compile under bazel test --nobuild_tests_only.
        _variant(name, source, profile, run, gpu, "ci_darwin_", ["external", "no-cache"] if name in fresh else [])
