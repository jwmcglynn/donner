#include "donner/css/FontFace.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace donner::css {

namespace {

/// Append a length-prefixed field, so a delimiter inside @p value cannot be read as a field
/// boundary and let two different declarations produce one key.
void appendField(std::string& key, std::string_view value) {
  key += std::to_string(value.size());
  key += ':';
  key += value;
}

/// Append a field holding the raw bytes of a pointer value.
void appendPointerField(std::string& key, const void* pointer) {
  // Formatted as a number rather than raw bytes so the key stays printable, and length-prefixed
  // like every other field so its width cannot shift the following fields.
  appendField(key, std::to_string(reinterpret_cast<uintptr_t>(pointer)));
}

/// ASCII-only lowercase, matching the case-insensitive family comparison used when selecting a
/// face. Locale-sensitive lowercasing would make the key depend on the process locale.
std::string toLowerAscii(std::string_view value) {
  std::string lowered(value);
  for (char& c : lowered) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return lowered;
}

}  // namespace

std::string FontFaceIdentityKey(const FontFace& face) {
  std::string key;
  appendField(key, toLowerAscii(std::string_view(face.familyName)));
  appendField(key, std::to_string(face.fontWeight));
  appendField(key, std::to_string(face.fontStyle));
  appendField(key, std::to_string(face.fontStretch));
  appendField(key, std::to_string(face.sources.size()));

  for (const FontFaceSource& source : face.sources) {
    appendField(key, std::to_string(static_cast<int>(source.kind)));
    appendField(key, source.trusted ? "T" : "U");
    appendField(key, std::string_view(source.formatHint));
    appendField(key, std::to_string(source.techHints.size()));
    for (const RcString& tech : source.techHints) {
      appendField(key, std::string_view(tech));
    }

    // Which alternative of the payload variant is held, tagged separately from its value so a
    // string payload can never read as a payload of another shape. These name the variant
    // alternative, not `FontFaceSource::Kind`; the kind is a separate field above, and a malformed
    // source can pair any kind with either alternative.
    if (const auto* text = std::get_if<RcString>(&source.payload)) {
      appendField(key, "payload-string");
      appendField(key, std::string_view(*text));
    } else if (const auto* data =
                   std::get_if<std::shared_ptr<const std::vector<uint8_t>>>(&source.payload)) {
      // Payload identity, not payload contents: hashing every byte would cost more than the load
      // this key exists to avoid, and the buffers are immutable and shared.
      appendField(key, "payload-bytes");
      appendPointerField(key, data->get());
    } else {
      // No alternative matched, which means the payload variant gained a case this key does not
      // know how to distinguish. Tag it so it cannot collapse onto one that is known.
      appendField(key, "payload-unknown");
    }
  }

  return key;
}

}  // namespace donner::css
