"""Shader families the production libraries link, and the artifact libraries they link them by.

Every family ships two library targets: `<family>_artifact`, the authored WGSL projection, and
`<family>_native_artifact`, the platform-native projection (MSL on Apple platforms, SPIR-V on
Linux). A production library links exactly one of them per family through
`linked_shader_artifacts`: the native projection on macOS and Linux, whose only production
devices are Metal and Vulkan, and the WGSL projection in the WebAssembly package, whose only
device is the browser's. C++ names that one artifact with `DONNER_LINKED_SHADER_ARTIFACT`.

A native test that drives production pipelines through a device consuming WGSL links the
test-only `//donner/gpu/shader:wgsl_alternate_projections`, which supplies the WGSL projections.
Test code that needs a specific projection names `shader_artifacts` or `native_shader_artifacts`.

The groupings below name the families each production library links, so the linkage probe and
the product dependency audits cover exactly the production set.
"""

# Families the transparency checkerboard pipeline links.
CHECKERBOARD_SHADER_FAMILIES = ["checkerboard"]

# Families the filter engine and the shared pipelines owned by the Geode device link.
GEODE_DEVICE_SHADER_FAMILIES = [
    "color_space_convert",
    "component_transfer",
    "composite",
    "convolve_matrix",
    "diffuse_lighting",
    "displacement_map",
    "drop_shadow",
    "filter_blend",
    "filter_color_matrix",
    "filter_image",
    "filter_resolve",
    "flood",
    "gaussian_blur",
    "image_blit",
    "merge",
    "morphology",
    "offset",
    "slug_fill",
    "slug_gradient",
    "slug_mask",
    "snapshot_unpremultiply",
    "specular_lighting",
    "subregion_clip",
    "tile",
    "turbulence",
]

# Families the Slug and image-blit module constructors link.
GEODE_MODULE_SHADER_FAMILIES = [
    "image_blit",
    "slug_fill",
    "slug_gradient",
    "slug_mask",
]

# The Slug text families. The geometry encoder links these, and so does the shader suite that
# exercises their module constructors.
SLUG_SHADER_FAMILIES = [
    "slug_fill",
    "slug_gradient",
    "slug_mask",
]

# Families the geometry encoder links.
GEODE_ENCODER_SHADER_FAMILIES = SLUG_SHADER_FAMILIES

# The family the editor's UI renderer links.
UI_DRAW_SHADER_FAMILIES = ["ui_draw"]

# Every family reachable from a production Geode library, each named once.
GEODE_SHADER_FAMILIES = sorted({
    family: None
    for family in (
        CHECKERBOARD_SHADER_FAMILIES +
        GEODE_DEVICE_SHADER_FAMILIES +
        GEODE_MODULE_SHADER_FAMILIES +
        GEODE_ENCODER_SHADER_FAMILIES
    )
}.keys())

# Every family reachable from a production library, the editor's included.
PRODUCTION_SHADER_FAMILIES = sorted(GEODE_SHADER_FAMILIES + UI_DRAW_SHADER_FAMILIES)

def shader_artifacts(families):
    """Returns the WGSL artifact libraries for `families`.

    Args:
        families: Shader family names.

    Returns:
        A list of `//donner/gpu/shader:<family>_artifact` labels.
    """
    return ["//donner/gpu/shader:%s_artifact" % family for family in families]

def shader_apis(families):
    """Returns the reflected host interface libraries for `families`.

    Args:
        families: Shader family names.

    Returns:
        A list of `//donner/gpu/shader:<family>_api` labels.
    """
    return ["//donner/gpu/shader:%s_api" % family for family in families]

def linked_shader_artifacts(families):
    """Returns the one artifact library per family a production library links.

    Args:
        families: Shader family names.

    Returns:
        A `select()` choosing `//donner/gpu/shader:<family>_native_artifact` on macOS and Linux
        and `//donner/gpu/shader:<family>_artifact` everywhere else, which is the WebAssembly
        package.
    """
    native = ["//donner/gpu/shader:%s_native_artifact" % family for family in families]
    return select({
        "@platforms//os:macos": native,
        "@platforms//os:linux": native,
        "//conditions:default": shader_artifacts(families),
    })

def wgsl_alternate_projections_for_tests():
    """Returns the test-only WGSL alternates where a native build needs them.

    A native test that drives production pipelines through a device consuming WGSL depends on
    this; the WebAssembly package links the WGSL artifacts already.

    Returns:
        A `select()` adding `//donner/gpu/shader:wgsl_alternate_projections` on macOS and Linux.
    """
    return select({
        "@platforms//os:macos": ["//donner/gpu/shader:wgsl_alternate_projections"],
        "@platforms//os:linux": ["//donner/gpu/shader:wgsl_alternate_projections"],
        "//conditions:default": [],
    })

def native_shader_artifacts(families):
    """Returns the platform-native artifact libraries for `families`, where they exist.

    Production libraries use `linked_shader_artifacts`; this is for tests and probes that need the
    native projection specifically.

    Args:
        families: Shader family names.

    Returns:
        A `select()` adding `//donner/gpu/shader:<family>_native_artifact` on the platforms with
        a native device, and nothing on the platforms without one.
    """
    labels = ["//donner/gpu/shader:%s_native_artifact" % family for family in families]
    return select({
        "@platforms//os:macos": labels,
        "@platforms//os:linux": labels,
        "//conditions:default": [],
    })
