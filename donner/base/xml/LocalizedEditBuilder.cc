#include "donner/base/xml/LocalizedEditBuilder.h"

#include <algorithm>

#include "donner/base/Utils.h"

namespace donner::xml {

namespace {

std::string serializeAttributes(const XMLNode& node) {
  std::string serialized;
  for (auto name : node.attributes()) {
    const std::string attrName = name.toString();
    std::optional<RcString> value = node.getAttribute(name);
    serialized.push_back(' ');
    serialized.append(attrName);
    serialized.append("=\"");
    if (value.has_value()) {
      serialized.append(std::string_view(*value));
    }
    serialized.push_back('"');
  }

  return serialized;
}

std::optional<FileOffsetRange> nodeRange(const XMLNode& node) {
  if (auto range = node.getNodeLocation()) {
    return range;
  }

  return std::nullopt;
}

}  // namespace

LocalizedEditBuilder::LocalizedEditBuilder(std::string_view source, std::string indentUnit)
    : source_(source), indentUnit_(std::move(indentUnit)) {}

std::optional<SourceDocument::Replacement> LocalizedEditBuilder::insertBeforeSibling(
    const XMLNode& node, const XMLNode& sibling) const {
  std::optional<FileOffsetRange> siblingRange = nodeRange(sibling);
  if (!siblingRange || !siblingRange->start.offset.has_value()) {
    return std::nullopt;
  }

  const size_t anchor = siblingRange->start.offset.value();
  const std::string indent = inferIndentation(anchor);
  std::string serialized = serializeNode(node, indent);
  if (isLineBreakBefore(anchor)) {
    if (serialized.starts_with(indent)) {
      serialized.erase(0, indent.size());
    }
    serialized.push_back('\n');
    serialized.append(indent);
  }

  return SourceDocument::Replacement{
      FileOffsetRange{FileOffset::Offset(anchor), FileOffset::Offset(anchor)},
      RcString(serialized)};
}

std::optional<SourceDocument::Replacement> LocalizedEditBuilder::appendChild(
    const XMLNode& node, const XMLNode& parent) const {
  std::optional<FileOffset> closingStart = closingTagStart(parent);
  if (!closingStart || !closingStart->offset.has_value()) {
    return std::nullopt;
  }

  const size_t anchor = closingStart->offset.value();
  const std::string indent = inferIndentation(anchor);
  std::string serialized = serializeNode(node, indent);
  if (!serialized.empty() && serialized.back() != '\n') {
    serialized.push_back('\n');
  }
  serialized.append(indent);

  return SourceDocument::Replacement{
      FileOffsetRange{FileOffset::Offset(anchor), FileOffset::Offset(anchor)},
      RcString(serialized)};
}

std::optional<SourceDocument::Replacement> LocalizedEditBuilder::removeNode(
    const XMLNode& node) const {
  std::optional<FileOffsetRange> range = nodeRange(node);
  if (!range) {
    return std::nullopt;
  }

  return SourceDocument::Replacement{*range, RcString("")};
}

std::string LocalizedEditBuilder::inferIndentation(size_t anchorOffset) const {
  if (source_.empty()) {
    return "";
  }

  const size_t cappedOffset = std::min(anchorOffset, source_.size() - 1);
  const size_t newlinePos = source_.rfind('\n', cappedOffset);
  const size_t indentStart = (newlinePos == std::string::npos) ? 0 : newlinePos + 1;
  size_t indentEnd = indentStart;
  while (indentEnd < anchorOffset && (source_[indentEnd] == ' ' || source_[indentEnd] == '\t')) {
    ++indentEnd;
  }

  return source_.substr(indentStart, indentEnd - indentStart);
}

bool LocalizedEditBuilder::isLineBreakBefore(size_t anchorOffset) const {
  if (source_.empty()) {
    return false;
  }

  size_t scanOffset = anchorOffset;
  while (scanOffset > 0 && (source_[scanOffset - 1] == ' ' || source_[scanOffset - 1] == '\t')) {
    --scanOffset;
  }

  if (scanOffset == 0) {
    return false;
  }

  return source_[scanOffset - 1] == '\n';
}

std::optional<FileOffset> LocalizedEditBuilder::closingTagStart(const XMLNode& node) const {
  std::optional<FileOffsetRange> range = nodeRange(node);
  if (!range || !range->start.offset.has_value() || !range->end.offset.has_value()) {
    return std::nullopt;
  }

  const size_t start = range->start.offset.value();
  const size_t end = range->end.offset.value();
  if (start >= source_.size() || end > source_.size() || start >= end) {
    return std::nullopt;
  }

  const std::string_view window(source_.data() + start, end - start);
  const size_t closingPos = window.rfind("</");
  if (closingPos != std::string_view::npos) {
    return FileOffset::Offset(start + closingPos);
  }

  const size_t selfClosingPos = window.rfind("/>");
  if (selfClosingPos != std::string_view::npos) {
    return FileOffset::Offset(start + selfClosingPos);
  }

  return std::nullopt;
}

std::string LocalizedEditBuilder::serializeNode(const XMLNode& node,
                                                std::string_view indent) const {
  const std::string attrs = serializeAttributes(node);
  switch (node.type()) {
    case XMLNode::Type::Document: return std::string();
    case XMLNode::Type::Data: {
      std::optional<RcString> value = node.value();
      std::string serialized(indent);
      if (value.has_value()) {
        serialized.append(std::string_view(*value));
      }
      return serialized;
    }
    case XMLNode::Type::CData: {
      std::optional<RcString> value = node.value();
      std::string serialized(indent);
      serialized.append("<![CDATA[");
      if (value.has_value()) {
        serialized.append(std::string_view(*value));
      }
      serialized.append("]]>");
      return serialized;
    }
    case XMLNode::Type::Comment: {
      std::optional<RcString> value = node.value();
      std::string serialized(indent);
      serialized.append("<!--");
      if (value.has_value()) {
        serialized.append(std::string_view(*value));
      }
      serialized.append("-->");
      return serialized;
    }
    case XMLNode::Type::DocType: {
      std::optional<RcString> value = node.value();
      std::string serialized(indent);
      serialized.append("<!DOCTYPE ");
      if (value.has_value()) {
        serialized.append(std::string_view(*value));
      }
      serialized.push_back('>');
      return serialized;
    }
    case XMLNode::Type::ProcessingInstruction:
    case XMLNode::Type::XMLDeclaration: {
      std::optional<RcString> value = node.value();
      const std::string target = node.tagName().toString();
      std::string serialized(indent);
      serialized.append("<?");
      serialized.append(target);
      if (value.has_value() && !std::string_view(*value).empty()) {
        serialized.push_back(' ');
        serialized.append(std::string_view(*value));
      }
      serialized.append("?>");
      return serialized;
    }
    case XMLNode::Type::Element: {
      const std::string tag = node.tagName().toString();
      const std::optional<RcString> value = node.value();
      const bool hasChildren = node.firstChild().has_value();
      std::string buffer;
      buffer.reserve(indent.size() + tag.size() + attrs.size() + 4);
      buffer.append(indent);
      buffer.push_back('<');
      buffer.append(tag);
      buffer.append(attrs);

      if (!value.has_value() && !hasChildren) {
        buffer.append("/>");
        return buffer;
      }

      buffer.push_back('>');
      if (value.has_value()) {
        buffer.append(std::string_view(*value));
      }

      if (hasChildren) {
        buffer.push_back('\n');
        const std::string childIndent = std::string(indent) + indentUnit_;
        for (auto child = node.firstChild(); child.has_value(); child = child->nextSibling()) {
          buffer.append(serializeNode(child.value(), childIndent));
          buffer.push_back('\n');
        }
        buffer.append(indent);
      }

      buffer.append("</");
      buffer.append(tag);
      buffer.push_back('>');
      return buffer;
    }
  }

  UTILS_UNREACHABLE();
}

}  // namespace donner::xml
