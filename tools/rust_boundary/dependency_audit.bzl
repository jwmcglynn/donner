"""Configured dependency audits for Donner's no-Rust production boundary."""

load("//build_defs:dep_audit.bzl", "configured_dependency_audit_test")

# These packages are test-only implementation boundaries. Product roots may
# use Donner's native or browser GPU implementations, but may not reach the
# WebGPU-C++/wgpu-native reference chain or the vendored Rust FFI oracle.
_FORBIDDEN_PRODUCTION_PACKAGES = [
    "//third_party/webgpu-cpp",
    "@tiny-skia-cpp//tests/rust_ffi",
    "@wgpu_native_linux_aarch64//",
    "@wgpu_native_linux_x86_64//",
]

def no_rust_dependency_audit_test(name, target, forbidden_packages = [], **kwargs):
    """Audit a selected product root for test-only Rust-backed dependencies."""
    configured_dependency_audit_test(
        name = name,
        target = target,
        forbidden_packages = _FORBIDDEN_PRODUCTION_PACKAGES + forbidden_packages,
        **kwargs
    )
