"""Shader families the production Geode libraries link, and their native artifacts.

Every family ships two library targets: `<family>_artifact`, the authored WGSL projection every
device accepts, and `<family>_native_artifact`, the platform-native projection (MSL on Apple
platforms, SPIR-V on Linux). A library that may run on a native device links both and selects
between them at runtime from `Device::shaderSourceKind()`. The WebAssembly package has no native
device, and `native_shader_artifacts` contributes nothing there, so that package keeps linking
the WGSL artifacts only.

The groupings below name the families each production library links, so the artifact probe that
asserts both projections reach a linked binary covers exactly the production set.
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

# Families the geometry encoder links.
GEODE_ENCODER_SHADER_FAMILIES = [
    "slug_fill",
    "slug_gradient",
    "slug_mask",
]

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

def native_shader_artifacts(families):
    """Returns the platform-native artifact libraries for `families`, where they exist.

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
