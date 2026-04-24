#pragma once
/// @file
///
/// Cosmetic helper that converts a URI or path into its preferred
/// address-bar display form. Paths that resolve inside the editor's
/// working directory get a `./` prefix so the bar reads as e.g.
/// `./donner_splash.svg` instead of `/Users/jwm/…/donner_splash.svg`.
/// Anything outside cwd, or any http(s)/data URI, passes through
/// unchanged.

#include <filesystem>
#include <string>
#include <string_view>

namespace donner::editor {

/// Returns \p uri in its preferred display form, using \p baseDir as
/// the "current directory" anchor. Lexical only — never touches the
/// filesystem — so it's deterministic in tests and safe on paths that
/// don't exist yet.
[[nodiscard]] std::string PrettifyLocalPath(std::string_view uri,
                                            const std::filesystem::path& baseDir);

}  // namespace donner::editor
