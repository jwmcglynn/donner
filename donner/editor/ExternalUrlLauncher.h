#pragma once
/// @file
/// Typed external destinations opened by the editor through platform APIs.

#include <string_view>

namespace donner::editor {

/// External destinations the editor is allowed to open.
enum class ExternalUrlTarget {
  DonnerRepository,
};

/// Resolve an allowlisted external destination to its HTTPS URL.
///
/// @param target Destination to resolve.
/// @return Stable process-lifetime URL string.
[[nodiscard]] std::string_view ExternalUrlValue(ExternalUrlTarget target);

/// Open an allowlisted destination with the platform URL handler.
///
/// @param target Destination to open.
/// @return True when the platform accepted the launch request.
[[nodiscard]] bool LaunchExternalUrl(ExternalUrlTarget target);

}  // namespace donner::editor
