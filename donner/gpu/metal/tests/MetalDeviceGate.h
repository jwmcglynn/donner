#pragma once
/// @file
/// Gates a Metal test fixture's SetUp() on having created a device: skip on a developer machine,
/// fail on an automated lane. Every Metal vertical slice in this package needs this, so it is a
/// shared macro rather than a third hand copy of the frozen pixel gate's fixture preamble.

#include <gtest/gtest.h>

#include <string>

#include "donner/gpu/baseline/FrozenBaselinePolicy.h"

/**
 * Gates a fixture's SetUp() on \p device having been created, ending the case when it was not:
 * skipped on a developer machine, failed on an automated lane.
 *
 * Use directly in SetUp(). Placed in a helper function, GTEST_SKIP()/FAIL() would return out of
 * the helper rather than SetUp(), and the fixture would go on to run its cases without a device.
 *
 * @param device Pointer-like value that is null when no device was created.
 * @param gateLabel What the gate is, for a reader who sees only this line.
 */
#define DONNER_REQUIRE_METAL_DEVICE(device, gateLabel)                                           \
  do {                                                                                           \
    if (!(device)) {                                                                             \
      const ::donner::gpu::baseline::MissingComparisonDisposition donnerMetalDeviceDisposition = \
          ::donner::gpu::baseline::DispositionForMissingAdapter(                                 \
              ::donner::gpu::baseline::RunningUnderContinuousIntegration());                     \
      const std::string donnerMetalDeviceMessage =                                               \
          ::donner::gpu::baseline::NoAdapterMessage((gateLabel), donnerMetalDeviceDisposition);  \
      if (donnerMetalDeviceDisposition ==                                                        \
          ::donner::gpu::baseline::MissingComparisonDisposition::FailClosed) {                   \
        FAIL() << donnerMetalDeviceMessage;                                                      \
      }                                                                                          \
      GTEST_SKIP() << donnerMetalDeviceMessage;                                                  \
    }                                                                                            \
  } while (false)
