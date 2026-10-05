#pragma once
/// @file
/// The program name of every shader family a production library links, for test code that must
/// visit each of them, and the program headers that declare their artifacts.

#include "donner/gpu/shader/programs/Checkerboard.h"
#include "donner/gpu/shader/programs/ColorSpaceConvert.h"
#include "donner/gpu/shader/programs/ComponentTransfer.h"
#include "donner/gpu/shader/programs/Composite.h"
#include "donner/gpu/shader/programs/ConvolveMatrix.h"
#include "donner/gpu/shader/programs/DiffuseLighting.h"
#include "donner/gpu/shader/programs/DisplacementMap.h"
#include "donner/gpu/shader/programs/DropShadow.h"
#include "donner/gpu/shader/programs/FilterBlend.h"
#include "donner/gpu/shader/programs/FilterColorMatrix.h"
#include "donner/gpu/shader/programs/FilterImage.h"
#include "donner/gpu/shader/programs/FilterResolve.h"
#include "donner/gpu/shader/programs/Flood.h"
#include "donner/gpu/shader/programs/GaussianBlur.h"
#include "donner/gpu/shader/programs/ImageBlit.h"
#include "donner/gpu/shader/programs/Merge.h"
#include "donner/gpu/shader/programs/Morphology.h"
#include "donner/gpu/shader/programs/Offset.h"
#include "donner/gpu/shader/programs/SlugFill.h"
#include "donner/gpu/shader/programs/SlugGradient.h"
#include "donner/gpu/shader/programs/SlugMask.h"
#include "donner/gpu/shader/programs/SnapshotUnpremultiply.h"
#include "donner/gpu/shader/programs/SpecularLighting.h"
#include "donner/gpu/shader/programs/SubregionClip.h"
#include "donner/gpu/shader/programs/Tile.h"
#include "donner/gpu/shader/programs/Turbulence.h"
#include "donner/gpu/shader/programs/UiDraw.h"

/**
 * Applies \p X to the program name of every family a production library links, as in
 * `X(SlugFill)`. Matches `PRODUCTION_SHADER_FAMILIES` in `shader_families.bzl`.
 *
 * @param X Macro taking one program name.
 */
#define DONNER_FOR_EACH_PRODUCTION_SHADER_FAMILY(X) \
  X(Checkerboard)                                   \
  X(ColorSpaceConvert)                              \
  X(ComponentTransfer)                              \
  X(Composite)                                      \
  X(ConvolveMatrix)                                 \
  X(DiffuseLighting)                                \
  X(DisplacementMap)                                \
  X(DropShadow)                                     \
  X(FilterBlend)                                    \
  X(FilterColorMatrix)                              \
  X(FilterImage)                                    \
  X(FilterResolve)                                  \
  X(Flood)                                          \
  X(GaussianBlur)                                   \
  X(ImageBlit)                                      \
  X(Merge)                                          \
  X(Morphology)                                     \
  X(Offset)                                         \
  X(SlugFill)                                       \
  X(SlugGradient)                                   \
  X(SlugMask)                                       \
  X(SnapshotUnpremultiply)                          \
  X(SpecularLighting)                               \
  X(SubregionClip)                                  \
  X(Tile)                                           \
  X(Turbulence)                                     \
  X(UiDraw)
