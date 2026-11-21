"""Native Linux hardware variants for the reviewed GPU-opening tests."""

def linux_gpu_variant(ci_platform_test, name, options):
    """Run the ordinary test and its runfiles on a Linux hardware TestRunner."""
    forwarded = {key: options[key] for key in ["args", "data", "flaky", "shard_count", "size", "timeout"] if key in options}
    ci_platform_test(
        name = "ci_linux_gpu_" + name,
        dep = ":" + name,
        target_platform = "//tools/ci:linux_ci_gpu",
        exec_compatible_with = ["@platforms//os:linux"],
        fixed_test_env = {
            "DONNER_REQUIRE_HARDWARE_GPU": "1",
            "VK_DRIVER_FILES": "/usr/share/vulkan/icd.d",
            "VK_ICD_FILENAMES": "/usr/share/vulkan/icd.d",
            "VK_LOADER_DRIVERS_DISABLE": "*lvp*",
            "XDG_RUNTIME_DIR": "/tmp",
        },
        tags = ["ci-linux-hardware-gpu", "manual", "external", "no-cache", "no-local"],
        visibility = ["//tools/ci:__pkg__"],
        **forwarded
    )

def linux_gpu_package_suite():
    """Expose exactly the hardware variants registered in this package."""
    members = [name for name, rule in native.existing_rules().items() if "ci-linux-hardware-gpu" in rule.get("tags", [])]
    if not members:
        fail("No Linux hardware GPU tests registered in " + native.package_name())
    native.test_suite(
        name = "ci_linux_gpu_tests",
        tests = members,
        tags = ["manual", "ci-linux-hardware-gpu"],
        visibility = ["//tools/ci:__pkg__"],
    )
