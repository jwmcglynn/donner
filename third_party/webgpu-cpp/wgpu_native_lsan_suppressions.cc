/// @file
/// LeakSanitizer suppression scoped to the prebuilt wgpu-native shared library.
///
/// wgpu-native ships as an opaque prebuilt binary (`libwgpu_native.so` on Linux). Creating and
/// then fully releasing a WebGPU instance/adapter/device leaves allocations behind inside that
/// library, and on Linux its Vulkan backend additionally unloads the loader/ICD it loaded at
/// runtime, which strands allocations those modules rooted in their own globals. LeakSanitizer
/// then reports unreachable blocks against whichever test process happened to create a device,
/// with stacks that mostly cannot be symbolized because the owning module is gone.
///
/// Attribution evidence: running the affected tests with `LSAN_OPTIONS=print_suppressions=1` and
/// this single `libwgpu_native.so` template accounts for every reported byte (for example
/// `//donner/gpu/shader:wgsl_emitter_geode_validation_tests`: 1303 allocations / 562632 bytes,
/// exactly the unsuppressed total). Every leaked chunk's allocation stack passes through the
/// prebuilt library. Donner's own teardown is not the cause: `GeodeDevice::~GeodeDevice` waits for
/// submitted work and then releases queue, device, adapter, and instance.
///
/// Matching by module name keeps leak detection fully enabled for Donner code. It is deliberately
/// narrower than disabling `detect_leaks` for whole packages, which is what the nightly sanitizer
/// job previously had to do.
///
/// LeakSanitizer matches a suppression against the function, file, and module of every frame in an
/// allocation stack, so this template covers any allocation the prebuilt library made, whatever
/// called into it.
///
/// It cannot cover allocations whose owning module is already unloaded when the leak check runs,
/// because those frames have no module name to match. The Vulkan loader unloads its driver
/// libraries during teardown and produces exactly that shape; `--config=asan` sets
/// `VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING` to keep them mapped. See the comment in
/// `.bazelrc`.

#if defined(__linux__) && !defined(__EMSCRIPTEN__)

/// Default LeakSanitizer suppression list, consulted by the sanitizer runtime at leak-check time.
/// The weak default in the runtime returns an empty list; this strong definition replaces it.
///
/// @return Newline-separated LeakSanitizer suppression templates.
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming)
extern "C" const char* __lsan_default_suppressions() {
  return "leak:libwgpu_native.so\n";
}

#endif
