/**
 * @example third_party_notice.cc Embed the third-party license notice
 * @details Prints the third-party license notice for the default build. The notice is aggregated by
 * `@donner//third_party/licenses:notice_default` and embedded into the binary at build time; an
 * application can show the same text behind an `--about` flag or a menu item. The build recipe is
 * in \ref GettingStarted.
 *
 * ```sh
 * bazel run //examples:third_party_notice
 * ```
 */

#include <iostream>
#include <span>
#include <string_view>

#include "embedded/notice_embedded.h"

//! [third_party_licenses]
std::string_view ThirdPartyLicenses() {
  const std::span<const unsigned char> notice = donner::embedded::kDonnerNotice;
  return std::string_view(reinterpret_cast<const char*>(notice.data()), notice.size());
}
//! [third_party_licenses]

int main() {
  std::cout << ThirdPartyLicenses();
  return 0;
}
