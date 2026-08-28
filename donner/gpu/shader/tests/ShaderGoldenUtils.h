#pragma once
/// @file
/// Golden-file helpers shared by the shader program tests.
///
/// Separate from ShaderTestUtils.h because reading a golden needs the runfiles lookup, and the
/// matcher helpers there are included by validation tests that have no test-data of their own.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "donner/base/tests/Runfiles.h"

namespace donner::gpu::shader {

/// Packs SPIR-V words little-endian, matching the on-disk golden encoding.
/// @param words Emitted SPIR-V module.
inline std::string SpirvWordsToBytes(const std::vector<uint32_t>& words) {
  std::string bytes;
  bytes.reserve(words.size() * 4);
  for (const uint32_t word : words) {
    bytes.push_back(static_cast<char>(word & 0xFF));
    bytes.push_back(static_cast<char>((word >> 8) & 0xFF));
    bytes.push_back(static_cast<char>((word >> 16) & 0xFF));
    bytes.push_back(static_cast<char>((word >> 24) & 0xFF));
  }
  return bytes;
}

/// Reads a committed golden from the shader test data directory.
/// @param name Golden file name under `donner/gpu/shader/tests/testdata`.
inline std::string ReadShaderGolden(const std::string& name) {
  const std::string path =
      donner::Runfiles::instance().Rlocation("donner/gpu/shader/tests/testdata/" + name);
  std::ifstream stream(path, std::ios::binary);
  EXPECT_TRUE(stream.good()) << "Failed to open golden file: " << path;
  std::ostringstream contents;
  contents << stream.rdbuf();
  return contents.str();
}

/// Rewrites a golden when its update environment variable names a repository root, and returns
/// true so the caller can skip the comparison.
///
/// Goldens are regenerated deliberately rather than on every run: a golden that rewrites itself
/// records whatever the emitter currently does instead of what it is supposed to do.
///
/// @param environmentVariable Name of the update variable, e.g. `UPDATE_WGSL_GOLDEN`.
/// @param name Golden file name under `donner/gpu/shader/tests/testdata`.
/// @param contents Bytes to write.
inline bool MaybeUpdateShaderGolden(const char* environmentVariable, const std::string& name,
                                    const std::string& contents) {
  const char* updateRoot = std::getenv(environmentVariable);
  if (updateRoot == nullptr) {
    return false;
  }
  const std::string outPath = std::string(updateRoot) + "/donner/gpu/shader/tests/testdata/" + name;
  std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
  EXPECT_TRUE(out.good()) << "Failed to open " << outPath << " for writing";
  out << contents;
  return true;
}

}  // namespace donner::gpu::shader
