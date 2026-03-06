cmake_minimum_required(VERSION 3.16)
project(tiny-skia-cpp LANGUAGES CXX)

# All library source files, mirroring the Bazel build structure.
set(TINY_SKIA_SOURCES
  # Core
  src/tiny_skia/AlphaRuns.cpp
  src/tiny_skia/BlendMode.cpp
  src/tiny_skia/Blitter.cpp
  src/tiny_skia/Color.cpp
  src/tiny_skia/Dash.cpp
  src/tiny_skia/Edge.cpp
  src/tiny_skia/EdgeBuilder.cpp
  src/tiny_skia/EdgeClipper.cpp
  src/tiny_skia/FixedPoint.cpp
  src/tiny_skia/FloatingPoint.cpp
  src/tiny_skia/Geom.cpp
  src/tiny_skia/LineClipper.cpp
  src/tiny_skia/Mask.cpp
  src/tiny_skia/Math.cpp
  src/tiny_skia/PathBuilder.cpp
  src/tiny_skia/PathGeometry.cpp
  src/tiny_skia/PathSegmentsIter.cpp
  src/tiny_skia/Pixmap.cpp
  src/tiny_skia/Stroke.cpp
  src/tiny_skia/Stroker.cpp
  src/tiny_skia/Transform.cpp

  # Pipeline
  src/tiny_skia/pipeline/Blitter.cpp
  src/tiny_skia/pipeline/Highp.cpp
  src/tiny_skia/pipeline/Lowp.cpp
  src/tiny_skia/pipeline/Mod.cpp

  # Shaders
  src/tiny_skia/shaders/Gradient.cpp
  src/tiny_skia/shaders/LinearGradient.cpp
  src/tiny_skia/shaders/Mod.cpp
  src/tiny_skia/shaders/Pattern.cpp
  src/tiny_skia/shaders/RadialGradient.cpp
  src/tiny_skia/shaders/SweepGradient.cpp

  # Scan
  src/tiny_skia/scan/Hairline.cpp
  src/tiny_skia/scan/HairlineAa.cpp
  src/tiny_skia/scan/Mod.cpp
  src/tiny_skia/scan/Path.cpp
  src/tiny_skia/scan/PathAa.cpp

  # Path64
  src/tiny_skia/path64/Cubic64.cpp
  src/tiny_skia/path64/LineCubicIntersections.cpp
  src/tiny_skia/path64/Mod.cpp
  src/tiny_skia/path64/Point64.cpp
  src/tiny_skia/path64/Quad64.cpp
  src/tiny_skia/path64/Scalar64.cpp

  # Wide (SIMD vector types)
  src/tiny_skia/wide/F32x4T.cpp
  src/tiny_skia/wide/F32x8T.cpp
  src/tiny_skia/wide/F32x16T.cpp
  src/tiny_skia/wide/I32x4T.cpp
  src/tiny_skia/wide/I32x8T.cpp
  src/tiny_skia/wide/U16x16T.cpp
  src/tiny_skia/wide/U32x4T.cpp
  src/tiny_skia/wide/U32x8T.cpp
  src/tiny_skia/wide/Mod.cpp

  # Painter (top-level drawing API)
  src/tiny_skia/MaskOps.cpp
  src/tiny_skia/Painter.cpp
)

# Helper: create a tiny-skia library target with the given SIMD mode.
function(add_tiny_skia_target TARGET_NAME SIMD_DEFINE)
  add_library(${TARGET_NAME} STATIC ${TINY_SKIA_SOURCES})

  target_include_directories(${TARGET_NAME} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
  )

  target_compile_features(${TARGET_NAME} PUBLIC cxx_std_20)

  target_compile_definitions(${TARGET_NAME} PRIVATE
    ${SIMD_DEFINE}
  )

  # Warnings.
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${TARGET_NAME} PRIVATE -Wall -ffp-contract=off)
  elseif(MSVC)
    target_compile_options(${TARGET_NAME} PRIVATE /W3 /fp:precise)
  endif()

  # Platform-specific SIMD flags for the native target.
  if(SIMD_DEFINE STREQUAL "TINYSKIA_CFG_IF_SIMD_NATIVE=1")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
      if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
        target_compile_options(${TARGET_NAME} PRIVATE -mavx2 -mfma)
      endif()
    elseif(MSVC)
      if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64")
        target_compile_options(${TARGET_NAME} PRIVATE /arch:AVX2)
      endif()
    endif()
  endif()
endfunction()

# Native SIMD target: uses AVX2+FMA on x86_64, NEON on ARM64, scalar fallback elsewhere.
add_tiny_skia_target(tiny_skia "TINYSKIA_CFG_IF_SIMD_NATIVE=1")

# Scalar-only target: portable, no platform SIMD intrinsics.
add_tiny_skia_target(tiny_skia_scalar "TINYSKIA_CFG_IF_SIMD_SCALAR=1")

# ── Examples (opt-in via -DTINY_SKIA_BUILD_EXAMPLES=ON) ──────────────────────
option(TINY_SKIA_BUILD_EXAMPLES "Build the tiny-skia-cpp example programs" OFF)

if(TINY_SKIA_BUILD_EXAMPLES)
  find_package(ZLIB REQUIRED)

  set(TINY_SKIA_EXAMPLES fill stroke hairline linear_gradient mask pattern gamma image_on_image large_image)
  foreach(example ${TINY_SKIA_EXAMPLES})
    add_executable(example_${example} examples/${example}.cpp)
    target_link_libraries(example_${example} PRIVATE tiny_skia ZLIB::ZLIB)
    target_include_directories(example_${example} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/examples
    )
  endforeach()
endif()

