#pragma once
/// @file

#include <optional>
#include <string>
#include <string_view>

namespace donner::svg {

/**
 * Resolve a URL reference against a base URL, following RFC 3986 section 5.2.
 *
 * This is used to resolve URLs that appear inside an externally loaded resource, such as an
 * `@import` inside an imported stylesheet, which resolve against that resource's own URL rather
 * than the document's.
 *
 * Unlike RFC 3986, the base may itself be a relative reference (for example
 * `../styles/main.css`), because Donner passes document-relative URLs to the caller's
 * \ref ResourceLoaderInterface, which resolves them against the document location. In that case
 * the result is also relative to the document, and leading `..` segments that cannot be removed
 * against the relative base are preserved instead of being dropped. An empty base represents the
 * document itself. Dot segments are removed from the result, so equivalent references produce the
 * same string.
 *
 * Resolution is a pure string transformation: it grants no access by itself. Every resolved URL is
 * still fetched through the caller's loader, which enforces its own policy (for example the
 * sandbox root of \ref SandboxedFileResourceLoader).
 *
 * @param base URL of the resource containing the reference, or empty for the document.
 * @param reference The reference to resolve.
 * @return The resolved URL, or `std::nullopt` when \p reference is relative and \p base is a
 *   non-hierarchical URL such as a `data:` URL.
 */
std::optional<std::string> ResolveUrlReference(std::string_view base, std::string_view reference);

}  // namespace donner::svg
