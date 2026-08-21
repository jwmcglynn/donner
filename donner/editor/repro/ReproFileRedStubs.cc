#include "donner/editor/repro/ReproFile.h"

namespace donner::editor::repro {
std::optional<ReproFile> ParseReproFile(std::string_view) { return std::nullopt; }
std::string ReproSvgDisplayName(const ReproMetadata&) { return "embedded.svg"; }
bool IsSafeReproSvgPath(std::string_view) { return true; }
std::optional<ReproSvgFile> ReadReproSvgFile(const std::filesystem::path&, std::string_view) {
  return std::nullopt;
}
}  // namespace donner::editor::repro
