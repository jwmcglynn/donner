#include "donner/svg/resources/UrlReference.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace donner::svg {

namespace {

/// Components of a URL reference, split per RFC 3986 appendix B.
struct UrlComponents {
  std::optional<std::string_view> scheme;
  std::optional<std::string_view> authority;
  std::string_view path;
  std::optional<std::string_view> query;
  std::optional<std::string_view> fragment;
};

bool IsSchemeChar(char ch, bool first) {
  const auto uch = static_cast<unsigned char>(ch);
  if (std::isalpha(uch) != 0) {
    return true;
  }
  return !first && (std::isdigit(uch) != 0 || ch == '+' || ch == '-' || ch == '.');
}

std::optional<std::string_view> SplitScheme(std::string_view& rest) {
  const std::size_t colon = rest.find_first_of(":/?#");
  if (colon == std::string_view::npos || colon == 0 || rest[colon] != ':') {
    return std::nullopt;
  }
  for (std::size_t i = 0; i < colon; ++i) {
    if (!IsSchemeChar(rest[i], i == 0)) {
      return std::nullopt;
    }
  }
  const std::string_view scheme = rest.substr(0, colon);
  rest.remove_prefix(colon + 1);
  return scheme;
}

/// Split the next component terminated by any of \p terminators, consuming it from \p rest.
std::string_view SplitUntil(std::string_view& rest, std::string_view terminators) {
  const std::size_t end = std::min(rest.find_first_of(terminators), rest.size());
  const std::string_view component = rest.substr(0, end);
  rest.remove_prefix(end);
  return component;
}

UrlComponents SplitUrl(std::string_view url) {
  UrlComponents result;
  std::string_view rest = url;
  result.scheme = SplitScheme(rest);
  if (rest.starts_with("//")) {
    rest.remove_prefix(2);
    result.authority = SplitUntil(rest, "/?#");
  }
  result.path = SplitUntil(rest, "?#");
  if (rest.starts_with('?')) {
    rest.remove_prefix(1);
    result.query = SplitUntil(rest, "#");
  }
  if (rest.starts_with('#')) {
    rest.remove_prefix(1);
    result.fragment = rest;
  }
  return result;
}

/**
 * Remove `.` and `..` segments (RFC 3986 section 5.2.4). For an absolute path, `..` cannot rise
 * above the root. For a relative path, unmatched `..` segments are kept so the path stays relative
 * to the same (unknown) base.
 */
std::string RemoveDotSegments(std::string_view path) {
  const bool absolute = path.starts_with('/');
  std::vector<std::string_view> segments;
  bool trailingDirectory = false;
  std::string_view rest = absolute ? path.substr(1) : path;
  while (true) {
    const std::size_t slash = rest.find('/');
    const std::string_view segment = rest.substr(0, slash);
    const bool last = slash == std::string_view::npos;
    trailingDirectory = false;
    if (segment == ".") {
      trailingDirectory = true;
    } else if (segment == "..") {
      if (!segments.empty() && segments.back() != "..") {
        segments.pop_back();
      } else if (!absolute) {
        segments.push_back(segment);
      }
      trailingDirectory = true;
    } else if (!last || !segment.empty()) {
      segments.push_back(segment);
    } else {
      trailingDirectory = !segments.empty();
    }
    if (last) {
      break;
    }
    rest.remove_prefix(slash + 1);
  }

  // A relative path whose first remaining segment is empty must not become root-absolute.
  std::string result = absolute ? "/" : (!segments.empty() && segments.front().empty() ? "./" : "");
  for (std::size_t i = 0; i < segments.size(); ++i) {
    if (i != 0) {
      result += '/';
    }
    result += segments[i];
  }
  if (trailingDirectory && !segments.empty()) {
    result += '/';
  }
  return result;
}

/// Merge a relative-path reference with the base path (RFC 3986 section 5.2.3).
std::string MergePaths(const UrlComponents& base, std::string_view referencePath) {
  if (base.authority.has_value() && base.path.empty()) {
    return "/" + std::string(referencePath);
  }
  const std::size_t lastSlash = base.path.rfind('/');
  if (lastSlash == std::string_view::npos) {
    return std::string(referencePath);
  }
  return std::string(base.path.substr(0, lastSlash + 1)) + std::string(referencePath);
}

/**
 * Keep a normalized path from changing how the recomposed URL parses (RFC 3986 section 4.2 and
 * 5.2.4): without an authority, a path starting with `//` would read as an authority, and without
 * a scheme, a first segment containing `:` would read as a scheme.
 */
std::string DisambiguatePath(std::string path, bool hasScheme, bool hasAuthority) {
  if (!hasAuthority && path.starts_with("//")) {
    return "/." + path;
  }
  if (!hasScheme && !hasAuthority && !path.starts_with('/')) {
    const std::size_t firstSegmentEnd = path.find('/');
    if (path.substr(0, firstSegmentEnd).find(':') != std::string::npos) {
      return "./" + path;
    }
  }
  return path;
}

std::string Recompose(std::optional<std::string_view> scheme,
                      std::optional<std::string_view> authority, std::string_view normalizedPath,
                      std::optional<std::string_view> query,
                      std::optional<std::string_view> fragment) {
  const std::string path =
      DisambiguatePath(std::string(normalizedPath), scheme.has_value(), authority.has_value());
  std::string result;
  if (scheme.has_value()) {
    result.append(*scheme).append(":");
  }
  if (authority.has_value()) {
    result.append("//").append(*authority);
  }
  result.append(path);
  if (query.has_value()) {
    result.append("?").append(*query);
  }
  if (fragment.has_value()) {
    result.append("#").append(*fragment);
  }
  return result;
}

bool IsOpaque(const UrlComponents& url) {
  return url.scheme.has_value() && !url.authority.has_value() && !url.path.starts_with('/');
}

}  // namespace

std::optional<std::string> ResolveUrlReference(std::string_view base, std::string_view reference) {
  const UrlComponents ref = SplitUrl(reference);
  if (IsOpaque(ref)) {
    // Opaque URLs such as `data:` have no path hierarchy; their contents are kept verbatim.
    return std::string(reference);
  }
  if (ref.scheme.has_value()) {
    return Recompose(ref.scheme, ref.authority, RemoveDotSegments(ref.path), ref.query,
                     ref.fragment);
  }

  const UrlComponents baseUrl = SplitUrl(base);
  if (IsOpaque(baseUrl)) {
    return std::nullopt;
  }
  if (ref.authority.has_value()) {
    return Recompose(baseUrl.scheme, ref.authority, RemoveDotSegments(ref.path), ref.query,
                     ref.fragment);
  }

  std::string path;
  std::optional<std::string_view> query = ref.query;
  if (ref.path.empty()) {
    path = RemoveDotSegments(baseUrl.path);
    if (!query.has_value()) {
      query = baseUrl.query;
    }
  } else if (ref.path.starts_with('/')) {
    path = RemoveDotSegments(ref.path);
  } else {
    path = RemoveDotSegments(MergePaths(baseUrl, ref.path));
  }
  return Recompose(baseUrl.scheme, baseUrl.authority, path, query, ref.fragment);
}

}  // namespace donner::svg
