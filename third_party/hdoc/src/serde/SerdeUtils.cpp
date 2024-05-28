// Copyright 2019-2023 hdoc
// SPDX-License-Identifier: AGPL-3.0-only

#include "SerdeUtils.hpp"

#include "spdlog/spdlog.h"

#include <fstream>
#include <sstream>
#include <streambuf>
#include <string>

void slurpFile(const std::filesystem::path& path, std::string& str) {
  std::ifstream t(path);
  if (!t.good()) {
    spdlog::error("Failed to open file: {}", path.string());
    return;
  }

  // Reserve space in the string to avoid reallocations during slurping
  t.seekg(0, std::ios::end);
  spdlog::warn("Slurping file: {} ({} bytes)", path.string(), static_cast<size_t>(t.tellg()));
  str.reserve(t.tellg());
  t.seekg(0, std::ios::beg);

  str.assign((std::istreambuf_iterator<char>(t)), std::istreambuf_iterator<char>());
}

bool dumpJSONPayload(const std::string_view filename, const std::string_view data) {
  std::ofstream out(std::string(filename.data(), filename.size()));
  if (!out) {
    spdlog::error("Failed to open {} file.", filename);
    return false;
  }

  out << data;
  spdlog::info("{} successfully written.", filename);
  return true;
}
