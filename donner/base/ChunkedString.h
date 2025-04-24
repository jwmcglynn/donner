#pragma once

#include <cassert>
#include <cstddef>
#include <ostream>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/base/RcStringOrRef.h"
#include "donner/base/SmallVector.h"
#include "donner/base/Utils.h"

namespace donner {

/**
 * ChunkedString is a small helper to accumulate multiple RcStringOrRef pieces,
 * either as small appended fragments or single codepoints. It can optionally
 * flatten them all into a single RcString if needed.
 */
class ChunkedString {
public:
  /**
   * Default constructor.
   */
  ChunkedString() = default;

  /**
   * Constructor from a string_view.
   * This is a zero-copy operation, referencing the underlying memory (if still alive).
   *
   * @param sv The string_view to initialize with.
   */
  explicit ChunkedString(std::string_view sv) { append(sv); }

  /**
   * Constructor from an RcString.
   *
   * @param str The RcString to initialize with.
   */
  explicit ChunkedString(const RcString& str) { append(str); }

  /**
   * Constructor from an RcStringOrRef.
   *
   * @param str The RcStringOrRef to initialize with.
   */
  explicit ChunkedString(const RcStringOrRef& str) { append(str); }

  /**
   * Constructor from a C-style string.
   *
   * @param str The C-style string to initialize with.
   */
  explicit ChunkedString(const char* str) { append(std::string_view(str)); }

  /**
   * Copy constructor from another ChunkedString.
   *
   * @param other The ChunkedString to copy from.
   */
  ChunkedString(const ChunkedString& other) {
    totalLength_ = other.totalLength_;
    pieces_ = other.pieces_;
  }

  /**
   * Assignment operator.
   *
   * @param other The ChunkedString to assign from.
   * @return A reference to this ChunkedString.
   */
  ChunkedString& operator=(const ChunkedString& other) {
    if (this != &other) {
      totalLength_ = other.totalLength_;
      pieces_ = other.pieces_;
    }
    return *this;
  }

  /**
   * Appends a string_view as a piece reference. This is a zero-copy operation,
   * referencing the underlying memory (if still alive).
   */
  void append(std::string_view sv) {
    totalLength_ += sv.size();
    pieces_.push_back(RcStringOrRef(sv));
  }

  /**
   * Helper method to append a C-style string literal.
   * This resolves the ambiguity between the string_view, RcString,
   * and RcStringOrRef overloads when passing string literals.
   *
   * @param str The string literal to append.
   */
  void appendLiteral(const char* str) { append(std::string_view(str)); }

  /**
   * Appends an RcStringOrRef by-value (moves in the reference).
   */
  void append(const RcStringOrRef& piece) {
    totalLength_ += piece.size();
    pieces_.push_back(piece);
  }

  /**
   * Appends an RcString.
   */
  void append(const RcString& piece) {
    totalLength_ += piece.size();
    pieces_.push_back(piece);
  }

  /**
   * Append another ChunkedString.
   */
  void append(const ChunkedString& other) {
    // Copy all pieces by-value.
    for (const auto& piece : other.pieces_) {
      append(RcString(piece));
    }
  }

  /**
   * Prepends a string_view at the beginning of the chunks.
   * This is a zero-copy operation, referencing the underlying memory (if still alive).
   */
  void prepend(std::string_view sv) {
    totalLength_ += sv.size();
    pieces_.insert(pieces_.begin(), RcStringOrRef(sv));
  }

  /**
   * Helper method to prepend a C-style string literal.
   * This resolves the ambiguity between the string_view, RcString,
   * and RcStringOrRef overloads when passing string literals.
   *
   * @param str The string literal to prepend.
   */
  void prependLiteral(const char* str) { prepend(std::string_view(str)); }

  /**
   * Prepends an RcStringOrRef by-value at the beginning of the chunks.
   */
  void prepend(const RcStringOrRef& piece) {
    totalLength_ += piece.size();
    pieces_.insert(pieces_.begin(), piece);
  }

  /**
   * Prepends an RcString at the beginning of the chunks.
   */
  void prepend(const RcString& piece) {
    totalLength_ += piece.size();
    pieces_.insert(pieces_.begin(), piece);
  }

  /**
   * Prepends another ChunkedString at the beginning of the chunks.
   */
  void prepend(const ChunkedString& other) {
    // We need to insert in reverse order to maintain the correct sequence
    // when inserting at the beginning
    for (size_t i = other.pieces_.size(); i > 0; --i) {
      totalLength_ += other.pieces_[i - 1].size();
      pieces_.insert(pieces_.begin(), RcString(other.pieces_[i - 1]));
    }
  }

  /**
   * Flattens all the pieces into one single RcStringOrRef. If there's only one
   * piece, returns it directly; if empty, returns an empty piece.
   * Otherwise, merges them into a newly allocated RcString and returns that.
   */
  RcString toSingleRcString() const {
    if (pieces_.empty()) {
      return RcString{};
    }

    if (pieces_.size() == 1) {
      return RcString(pieces_[0]);
    }

    // Multiple pieces => flatten
    std::vector<char> buffer;
    buffer.reserve(totalLength_);

    for (const auto& piece : pieces_) {
      buffer.insert(buffer.end(), piece.begin(), piece.end());
    }

    return RcString::fromVector(std::move(buffer));
  }

  /**
   * Return the first chunk as a string_view.
   */
  std::string_view firstChunk() const {
    if (pieces_.empty()) {
      return std::string_view();
    }

    return std::string_view(pieces_[0]);
  }

  /**
   * Return the total length of the string.
   */
  size_t size() const { return totalLength_; }

  /**
   * Return the number of chunks in the string.
   */
  size_t numChunks() const { return pieces_.size(); }

  /**
   * Checks if the string is empty.
   *
   * @return true if the string is empty, false otherwise.
   */
  bool empty() const { return totalLength_ == 0; }

  /**
   * Access the character at the specified position.
   * Note: This operation may be slow for large strings with many chunks,
   * as it needs to find the chunk containing the specified position.
   *
   * @param pos Position of the character to access.
   * @return The character at the specified position.
   * @note Behavior is undefined if pos is out of range.
   */
  char operator[](size_t pos) const {
    assert(pos < totalLength_ && "Index out of bounds");

    size_t currentPos = 0;
    for (const auto& piece : pieces_) {
      if (pos < currentPos + piece.size()) {
        // We need to convert the RcStringOrRef to std::string_view to access by index
        std::string_view sv = piece;
        return sv[pos - currentPos];
      }
      currentPos += piece.size();
    }

    UTILS_UNREACHABLE();
  }

  /**
   * Returns a substring of this string.
   *
   * @param pos Position of the first character to include.
   * @param count Number of characters to include.
   * @return A new string containing the specified substring.
   * @note Behavior is undefined if pos is out of range.
   */
  ChunkedString substr(size_t pos, size_t count = std::string_view::npos) const {
    assert(pos <= totalLength_ && "Position out of range");

    if (pos == totalLength_) {
      // Special case: empty substring at the end
      return ChunkedString();
    }

    // If we just have a single chunk, delegate to its substr method
    if (pieces_.size() == 1) {
      return ChunkedString(pieces_[0].substr(pos, count));
    }

    // Handle substring across multiple chunks efficiently
    ChunkedString result;

    // Limit count to the available characters
    if (count == std::string_view::npos) {
      count = totalLength_ - pos;
    }

    assert(pos + count <= totalLength_ && "Substring length out of range");

    // Find the starting chunk and offset
    size_t currentPos = 0;
    size_t remaining = count;

    // Locate the chunk containing the start position
    for (size_t i = 0; i < pieces_.size() && remaining > 0; ++i) {
      const RcStringOrRef& piece = pieces_[i];
      const size_t chunkSize = piece.size();

      // Skip chunks before the start position
      if (pos >= currentPos + chunkSize) {
        currentPos += chunkSize;
        continue;
      }

      // Calculate the offset within this chunk
      const size_t offset = pos > currentPos ? pos - currentPos : 0;
      const size_t available = chunkSize - offset;
      const size_t toTake = std::min(available, remaining);

      // Add this piece to the result
      result.append(pieces_[i].substr(offset, toTake));

      remaining -= toTake;
      currentPos += chunkSize;
    }

    return result;
  }

  /**
   * Removes the first n characters from the string.
   *
   * @param n Number of characters to remove.
   */
  void remove_prefix(size_t n) {
    if (n == 0) {
      return;
    }

    if (n >= totalLength_) {
      // Remove all content
      pieces_.clear();
      totalLength_ = 0;
      return;
    }

    size_t remainingToRemove = n;
    while (!pieces_.empty() && remainingToRemove > 0) {
      const auto& piece = pieces_[0];

      if (remainingToRemove >= piece.size()) {
        // Remove the entire chunk
        remainingToRemove -= piece.size();
        totalLength_ -= piece.size();

        // Remove the first piece
        // Create a new vector without the first piece
        SmallVector<RcStringOrRef, 5> newPieces;
        for (size_t i = 1; i < pieces_.size(); ++i) {
          newPieces.push_back(pieces_[i]);
        }
        pieces_ = std::move(newPieces);
      } else {
        // Remove part of the first chunk
        RcStringOrRef newPiece = pieces_[0].substr(remainingToRemove);
        totalLength_ -= remainingToRemove;

        pieces_[0] = newPiece;
        break;
      }
    }
  }

  /**
   * Checks if the string starts with the given prefix.
   *
   * @param prefix The prefix to check for.
   * @return true if the string starts with the prefix, false otherwise.
   */
  bool starts_with(std::string_view prefix) const {
    if (prefix.empty()) {
      return true;  // Empty prefix is always a prefix of any string
    }

    if (totalLength_ < prefix.size()) {
      return false;  // String is shorter than the prefix
    }

    // If we only have a single chunk, delegate to its starts_with method
    if (pieces_.size() == 1) {
      return std::string_view(pieces_[0]).starts_with(prefix);
    }

    // For multiple chunks, we need to check character by character
    size_t prefixPos = 0;
    size_t stringPos = 0;

    for (const auto& piece : pieces_) {
      std::string_view pieceView = piece;

      for (size_t i = 0; i < pieceView.size() && prefixPos < prefix.size(); ++i, ++stringPos) {
        if (pieceView[i] != prefix[prefixPos++]) {
          return false;
        }

        if (prefixPos == prefix.size()) {
          return true;  // Found the complete prefix
        }
      }
    }

    UTILS_UNREACHABLE();  // This would mean that we ran out of characters before finding the
                          // prefix, but we already checked the size above.
  }

  /**
   * Equality operator to compare with another ChunkedString.
   *
   * @param other The ChunkedString to compare with.
   * @return true if the strings are equal, false otherwise.
   */
  bool operator==(const ChunkedString& other) const {
    // Quick check on size
    if (totalLength_ != other.totalLength_) {
      return false;
    }

    // Both are empty
    if (totalLength_ == 0) {
      return true;
    }

    // If both have a single chunk, compare them directly
    if (pieces_.size() == 1 && other.pieces_.size() == 1) {
      return std::string_view(pieces_[0]) == std::string_view(other.pieces_[0]);
    }

    // Otherwise, flatten and compare
    return toSingleRcString() == other.toSingleRcString();
  }

  /**
   * Equality operator to compare with a string_view.
   *
   * @param sv The string_view to compare with.
   * @return true if the strings are equal, false otherwise.
   */
  bool operator==(std::string_view sv) const {
    // Quick check on size
    if (totalLength_ != sv.size()) {
      return false;
    }

    // Both are empty
    if (totalLength_ == 0) {
      return true;
    }

    // If we have a single chunk, compare directly
    if (pieces_.size() == 1) {
      return std::string_view(pieces_[0]) == sv;
    }

    // Otherwise, flatten and compare
    return toSingleRcString() == sv;
  }

  /**
   * Equality operator to compare with an RcString.
   *
   * @param str The RcString to compare with.
   * @return true if the strings are equal, false otherwise.
   */
  bool operator==(const RcString& str) const { return (*this) == std::string_view(str); }

  /**
   * Equality operator to compare with an RcStringOrRef.
   *
   * @param str The RcStringOrRef to compare with.
   * @return true if the strings are equal, false otherwise.
   */
  bool operator==(const RcStringOrRef& str) const { return (*this) == std::string_view(str); }

  /**
   * Equality operator to compare with a C-style string.
   *
   * @param str The C-style string to compare with.
   * @return true if the strings are equal, false otherwise.
   */
  bool operator==(const char* str) const { return (*this) == std::string_view(str); }

  /**
   * Friend functions to allow reversed equality syntax.
   */
  friend bool operator==(std::string_view sv, const ChunkedString& cs) { return cs == sv; }

  friend bool operator==(const RcString& str, const ChunkedString& cs) { return cs == str; }

  friend bool operator==(const RcStringOrRef& str, const ChunkedString& cs) { return cs == str; }

  friend bool operator==(const char* str, const ChunkedString& cs) { return cs == str; }

  /**
   * Stream insertion operator.
   *
   * @param os The output stream.
   * @param cs The ChunkedString to output.
   * @return The output stream.
   */
  friend std::ostream& operator<<(std::ostream& os, const ChunkedString& cs) {
    for (const auto& piece : cs.pieces_) {
      os << std::string_view(piece);
    }
    return os;
  }

  /**
   * Checks if the string ends with the given suffix.
   *
   * @param suffix The suffix to check for.
   * @return true if the string ends with the suffix, false otherwise.
   */
  bool ends_with(std::string_view suffix) const {
    if (suffix.empty()) {
      return true;  // Empty suffix is always a suffix of any string
    }

    if (totalLength_ < suffix.size()) {
      return false;  // String is shorter than the suffix
    }

    // If we only have a single chunk, delegate to string_view
    if (pieces_.size() == 1) {
      std::string_view sv = pieces_[0];
      return sv.substr(sv.size() - suffix.size()) == suffix;
    }

    // For multiple chunks, we need a more complex approach
    // We'll work backwards from the end of our string

    // First, let's find which chunk contains the start of the potential suffix
    size_t suffixStart = totalLength_ - suffix.size();
    size_t currentPos = 0;
    size_t chunkIndex = 0;

    // Find the chunk containing the start of the suffix
    while (chunkIndex < pieces_.size()) {
      size_t nextPos = currentPos + pieces_[chunkIndex].size();
      if (suffixStart < nextPos) {
        break;  // Found the chunk containing the start position
      }
      currentPos = nextPos;
      chunkIndex++;
    }

    // Now we check character by character from the suffix start
    size_t suffixPos = 0;
    size_t offsetInChunk = suffixStart - currentPos;

    for (; chunkIndex < pieces_.size(); ++chunkIndex) {
      std::string_view pieceView = pieces_[chunkIndex];

      for (size_t i = offsetInChunk; i < pieceView.size(); ++i) {
        if (pieceView[i] != suffix[suffixPos++]) {
          return false;
        }

        if (suffixPos == suffix.size()) {
          return true;  // We've matched the entire suffix
        }
      }

      // Reset for the next chunk
      offsetInChunk = 0;
    }

    UTILS_UNREACHABLE();  // This would mean that we ran out of characters before finding the
                          // suffix, but we already checked the size above.
  }

  /**
   * Finds the first occurrence of a substring.
   *
   * @param s The substring to find.
   * @param pos The position to start searching from.
   * @return The position of the found substring, or npos if not found.
   */
  size_t find(std::string_view s, size_t pos = 0) const {
    static constexpr size_t npos = std::string_view::npos;

    if (s.empty()) {
      return pos <= totalLength_ ? pos : npos;  // Empty string is found at pos if pos is valid
    }

    if (pos >= totalLength_) {
      return npos;  // pos is beyond the end of the string
    }

    if (pos + s.size() > totalLength_) {
      return npos;  // Not enough characters left
    }

    // If we only have a single chunk, delegate to its find method
    if (pieces_.size() == 1) {
      size_t result = std::string_view(pieces_[0]).find(s, pos);
      return result != npos ? result : npos;
    }

    // For multiple chunks, we need to flatten the string
    RcString flattened = toSingleRcString();
    return std::string_view(flattened).find(s, pos);
  }

private:
  SmallVector<RcStringOrRef, 5> pieces_;
  size_t totalLength_ = 0;
};

}  // namespace donner
