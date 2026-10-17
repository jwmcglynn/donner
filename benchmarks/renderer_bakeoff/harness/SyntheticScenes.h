#pragma once
/// @file
/// Deterministically generated stress scenes, so large geometry loads need no checked-in files.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace donner::bakeoff {

/// Names of every synthetic scene, in a stable order.
std::vector<std::string_view> SyntheticSceneNames();

/// Generates the SVG source of a synthetic scene. Output is identical across runs and hosts.
/// @param name Scene name from \ref SyntheticSceneNames.
/// @return SVG text, or nullopt for an unknown name.
std::optional<std::string> GenerateSyntheticScene(std::string_view name);

}  // namespace donner::bakeoff
