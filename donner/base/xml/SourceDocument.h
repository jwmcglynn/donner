#pragma once
/// @file

#include <cstdint>
#include <string_view>
#include <vector>

#include "donner/base/FileOffset.h"
#include "donner/base/ParseResult.h"
#include "donner/base/RcString.h"
#include "donner/base/RcStringOrRef.h"
#include "donner/base/parser/LineOffsets.h"

namespace donner::xml {

/**
 * Immutable view of an XML source buffer that supports constrained span-based replacements.
 */
class SourceDocument {
public:
  /**
   * A replacement to apply to the source text.
   */
  struct Replacement {
    FileOffsetRange range;  ///< Original span to replace.
    RcString replacement;   ///< Replacement text.
  };

    /**
     * Maps offsets from the original source to the updated source after replacements.
     */
    class OffsetMap {
    public:
      struct ReplacementInfo {
        size_t start;
        size_t end;
        size_t replacementSize;
        int64_t deltaBefore;
        int64_t deltaAfter;
      };

      OffsetMap() = default;

      OffsetMap(size_t originalSize, std::vector<ReplacementInfo>&& replacements,
                parser::LineOffsets&& lineOffsets);

      /// Translate an offset from the original text into the updated text.
      FileOffset translateOffset(const FileOffset& offset) const;

      /// Translate a range from the original text into the updated text.
      FileOffsetRange translateRange(const FileOffsetRange& range) const;

    private:
      size_t mapOffset(size_t offset) const;

      size_t originalSize_ = 0;
      std::vector<ReplacementInfo> replacements_;
      parser::LineOffsets lineOffsets_ = parser::LineOffsets("");
    };

  struct ApplyResult {
    ApplyResult(RcString text, OffsetMap offsetMap);

    RcString text;  ///< Updated source text after replacements.
    OffsetMap offsetMap;
  };

  explicit SourceDocument(const RcStringOrRef& text);

  /// Access the immutable original text.
  std::string_view originalText() const { return source_; }

  /// Length of the original source.
  size_t size() const { return source_.size(); }

  /**
   * Apply the given non-overlapping replacements and return the updated text and offset map.
   * Replacements must be ordered by start offset and may not overlap.
   */
  ParseResult<ApplyResult> applyReplacements(const std::vector<Replacement>& replacements) const;

private:
  RcString source_;
};

}  // namespace donner::xml
