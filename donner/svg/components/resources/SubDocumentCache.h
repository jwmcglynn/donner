#pragma once
/// @file

#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "donner/base/ParseError.h"
#include "donner/base/RcString.h"
#include "donner/svg/SVGDocumentHandle.h"

namespace donner::svg::components {

/**
 * Cache for parsed SVG sub-documents referenced by `<image>` or `<use>` elements.
 *
 * Sub-documents are parsed in \ref ProcessingMode::SecureStatic mode, which prevents them from
 * loading their own external resources (per SVG2 §2.7.1). This prevents infinite recursion when
 * document A references document B which references document A.
 *
 * This stores \ref SVGDocumentHandle values, which are the same shared internal state used by
 * \ref SVGDocument's by-value facade.
 */
class SubDocumentCache {
public:
  /**
   * Callback type for parsing SVG content into a document. Called by \ref getOrParse when the URL
   * is not cached. Should return an \ref SVGDocumentHandle on success, or `std::nullopt` on
   * failure.
   */
  using ParseCallback = std::function<std::optional<SVGDocumentHandle>(
      const std::vector<uint8_t>& svgContent, std::vector<ParseError>* outWarnings)>;

  /// Constructor.
  SubDocumentCache() = default;

  /// Destructor.
  ~SubDocumentCache() = default;

  // Non-copyable, movable.
  SubDocumentCache(const SubDocumentCache&) = delete;
  SubDocumentCache& operator=(const SubDocumentCache&) = delete;
  SubDocumentCache(SubDocumentCache&&) = default;
  SubDocumentCache& operator=(SubDocumentCache&&) = default;

  /**
   * Get a previously cached sub-document, or parse and cache a new one from raw SVG bytes.
   *
   * If the URL is already being loaded (circular reference), returns `std::nullopt`.
   *
   * @param resolvedUrl The resolved URL used as the cache key.
   * @param svgContent Raw SVG document bytes to parse if not already cached.
   * @param parseCallback Callback to parse SVG content into a document.
   * @param outWarnings If non-null, append parse warnings to this vector.
   * @return Cached document-state handle, or `std::nullopt` on failure.
   */
  std::optional<SVGDocumentHandle> getOrParse(const RcString& resolvedUrl,
                                              const std::vector<uint8_t>& svgContent,
                                              const ParseCallback& parseCallback,
                                              std::vector<ParseError>* outWarnings);

  /**
   * Get a previously cached sub-document by URL.
   *
   * @param resolvedUrl The resolved URL to look up.
   * @return Cached document-state handle, or `std::nullopt` if not found.
   */
  std::optional<SVGDocumentHandle> get(const RcString& resolvedUrl) const;

  /// Returns true if the given URL is currently being loaded (for recursion detection).
  bool isLoading(const RcString& resolvedUrl) const;

  /// Returns the number of cached sub-documents.
  size_t size() const { return cache_.size(); }

private:
  /// Cached sub-documents keyed by resolved URL.
  std::unordered_map<RcString, SVGDocumentHandle> cache_;

  /// URLs currently being loaded, used to detect circular references.
  std::unordered_set<RcString> loading_;
};

}  // namespace donner::svg::components
