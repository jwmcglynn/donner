"""
cargo-raze crate build file.

DO NOT EDIT! Replaced on runs of cargo-raze
"""
package(default_visibility = [
  # Public for visibility by "@raze__crate__version//" targets.
  #
  # Prefer access through "//third_party/pathfinder", which limits external
  # visibility to explicit Cargo.toml dependencies.
  "//visibility:public",
])

licenses([
  "restricted", # "FTL,GPL-2.0"
])

load(
    "@io_bazel_rules_rust//rust:rust.bzl",
    "rust_library",
    "rust_binary",
    "rust_test",
)


# Unsupported target "build-script-build" with type "custom-build" omitted
alias(
  name = "servo_freetype_sys",
  actual = ":freetype_sys",
)

rust_library(
    name = "freetype_sys",
    crate_root = "src/lib.rs",
    crate_type = "lib",
    edition = "2015",
    srcs = glob(["**/*.rs"]),
    deps = [
        ":freetype2",
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    version = "4.0.5",
    crate_features = [
    ],
)


# Additional content from servo_freetype_sys_patch.BUILD
load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "freetype2",
    srcs = [
        "freetype2/src/autofit/autofit.c",
        "freetype2/src/base/ftbase.c",
        "freetype2/src/base/ftbbox.c",
        "freetype2/src/base/ftbdf.c",
        "freetype2/src/base/ftbitmap.c",
        "freetype2/src/base/ftcid.c",
        "freetype2/src/base/ftfntfmt.c",
        "freetype2/src/base/ftfstype.c",
        "freetype2/src/base/ftgasp.c",
        "freetype2/src/base/ftglyph.c",
        "freetype2/src/base/ftgxval.c",
        "freetype2/src/base/ftinit.c",
        "freetype2/src/base/ftlcdfil.c",
        "freetype2/src/base/ftmm.c",
        "freetype2/src/base/ftotval.c",
        "freetype2/src/base/ftpatent.c",
        "freetype2/src/base/ftpfr.c",
        "freetype2/src/base/ftstroke.c",
        "freetype2/src/base/ftsynth.c",
        "freetype2/src/base/ftsystem.c",
        "freetype2/src/base/fttype1.c",
        "freetype2/src/base/ftwinfnt.c",
        "freetype2/src/bdf/bdf.c",
        "freetype2/src/bzip2/ftbzip2.c",
        "freetype2/src/cache/ftcache.c",
        "freetype2/src/cff/cff.c",
        "freetype2/src/cid/type1cid.c",
        "freetype2/src/gzip/ftgzip.c",
        "freetype2/src/lzw/ftlzw.c",
        "freetype2/src/pcf/pcf.c",
        "freetype2/src/pfr/pfr.c",
        "freetype2/src/psaux/psaux.c",
        "freetype2/src/pshinter/pshinter.c",
        "freetype2/src/psnames/psnames.c",
        "freetype2/src/raster/raster.c",
        "freetype2/src/sfnt/sfnt.c",
        "freetype2/src/smooth/smooth.c",
        "freetype2/src/truetype/truetype.c",
        "freetype2/src/type1/type1.c",
        "freetype2/src/type42/type42.c",
        "freetype2/src/winfonts/winfnt.c",
        "freetype2/src/base/ftdebug.c",
    ] + glob([
        "freetype2/src/**/*.h",
        "freetype2/builds/unix/*.h",
        "freetype2/include/freetype/internal/**/*.h",
    ]),
    hdrs = glob([
        "freetype2/include/freetype/*.h",
        "freetype2/include/freetype/config/*.h",
        "freetype2/src/**/*.c",
    ]) + [
        "freetype2/include/ft2build.h",
    ],
    copts = [
        "-Wno-covered-switch-default",
        "-DFT_CONFIG_CONFIG_H=\\\"third_party/pathfinder_wrapper/freetype2/ftconfig.h\\\"",
        "-DFT2_BUILD_LIBRARY",
        "-DFT_CONFIG_MODULES_H=\\\"third_party/pathfinder_wrapper/freetype2/ftmodule.h\\\"",
        "-DHAVE_UNISTD_H=1",
        "-DHAVE_FCNTL_H=1",
        "-DHAVE_STDINT_H=1",
        "-Ifreetype2/builds/unix",
        "-Ifreetype2/include",
        "-Ifreetype2/include/freetype/config",
    ],
    includes = ["freetype2/include"],
    visibility = ["//visibility:public"],
    deps = [
        "@//third_party/pathfinder_wrapper/freetype2:freetype2_config",
    ],
)