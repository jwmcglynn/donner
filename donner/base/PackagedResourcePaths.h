#pragma once
/// @file

#include <filesystem>
#include <string>

namespace donner::detail {

/// Select package data relative to a canonical native executable path.
/// A present executable-specific entry is authoritative, even when it is invalid.
std::string InstalledResourceRootForExecutable(const std::filesystem::path& executable);

}  // namespace donner::detail
