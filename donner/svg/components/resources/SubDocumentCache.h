#pragma once
/// @file

#include <any>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "donner/base/ParseError.h"
#include "donner/base/RcString.h"

namespace donner::svg::components {

/**
 * Cache for parsed SVG sub-documents referenced by `<image>`, `<use>`, or `<feImage>` elements.
 *
 * Sub-documents are parsed in \ref ProcessingMode::SecureStatic mode, which prevents them from
 * loading their own external resources (per SVG2 §2.7.1). This prevents infinite recursion when
 * document A references document B which references document A.
 *
 * Documents are stored type-erased (via `std::any`) to avoid circular build dependencies between
 * the component layer and `SVGDocument`/`SVGParser`. The actual type stored is `SVGDocument`.
 *
 * This is stored as an ECS context component (`Registry::ctx()`) on the parent document's registry.
 */
class SubDocumentCache {
public:
  /**
   * Callback type for parsing SVG content into a document. Called by \ref getOrParse when the URL
   * is not cached. Should return an `std::any` wrapping an `SVGDocument` on success, or
   * `std::nullopt` on failure.
   */
  using ParseCallback = std::function<std::optional<std::any>(
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
   * If the URL is already being loaded (circular reference), returns nullptr.
   * The returned pointer is a type-erased `SVGDocument*` — callers must `std::any_cast` to access
   * the document.
   *
   * @param resolvedUrl The resolved URL used as the cache key.
   * @param svgContent Raw SVG document bytes to parse if not already cached.
   * @param parseCallback Callback to parse SVG content into a document.
   * @param outWarnings If non-null, append parse warnings to this vector.
   * @return Pointer to the cached `std::any` (containing `SVGDocument`), or nullptr on failure.
   */
  std::any* getOrParse(const RcString& resolvedUrl, const std::vector<uint8_t>& svgContent,
                       const ParseCallback& parseCallback,
                       std::vector<ParseError>* outWarnings);

  /**
   * Get a previously cached sub-document by URL.
   *
   * @param resolvedUrl The resolved URL to look up.
   * @return Pointer to the cached `std::any`, or nullptr if not found.
   */
  std::any* get(const RcString& resolvedUrl);

  /// Returns true if the given URL is currently being loaded (for recursion detection).
  bool isLoading(const RcString& resolvedUrl) const;

  /// Returns the number of cached sub-documents.
  size_t size() const { return cache_.size(); }

private:
  /// Cached sub-documents keyed by resolved URL.
  std::unordered_map<RcString, std::any> cache_;

  /// URLs currently being loaded, used to detect circular references.
  std::unordered_set<RcString> loading_;
};

}  // namespace donner::svg::components
