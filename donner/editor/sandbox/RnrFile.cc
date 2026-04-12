#include "donner/editor/sandbox/RnrFile.h"

#include <cstring>
#include <fstream>

namespace donner::editor::sandbox {

namespace {

template <typename T>
void AppendPod(std::vector<uint8_t>& dst, const T& value) {
  static_assert(std::is_trivially_copyable_v<T>);
  const std::size_t offset = dst.size();
  dst.resize(offset + sizeof(T));
  std::memcpy(dst.data() + offset, &value, sizeof(T));
}

template <typename T>
bool ReadPod(std::span<const uint8_t>& cursor, T& out) {
  static_assert(std::is_trivially_copyable_v<T>);
  if (cursor.size() < sizeof(T)) return false;
  std::memcpy(&out, cursor.data(), sizeof(T));
  cursor = cursor.subspan(sizeof(T));
  return true;
}

}  // namespace

std::vector<uint8_t> EncodeRnrBuffer(const RnrHeader& header,
                                     std::span<const uint8_t> wireBytes) {
  std::vector<uint8_t> buf;
  buf.reserve(28 + header.uri.size() + wireBytes.size());

  AppendPod<uint32_t>(buf, kRnrFileMagic);
  AppendPod<uint32_t>(buf, header.fileVersion);
  AppendPod<uint64_t>(buf, header.timestampNanos);
  AppendPod<uint32_t>(buf, header.width);
  AppendPod<uint32_t>(buf, header.height);
  AppendPod<uint32_t>(buf, static_cast<uint32_t>(header.backend));
  AppendPod<uint32_t>(buf, static_cast<uint32_t>(header.uri.size()));
  buf.insert(buf.end(), header.uri.begin(), header.uri.end());
  buf.insert(buf.end(), wireBytes.begin(), wireBytes.end());
  return buf;
}

RnrIoStatus ParseRnrBuffer(std::span<const uint8_t> buffer, RnrHeader& outHeader,
                           std::vector<uint8_t>& outWireBytes) {
  outHeader = RnrHeader{};
  outWireBytes.clear();

  std::span<const uint8_t> cursor = buffer;
  uint32_t magic = 0;
  if (!ReadPod(cursor, magic)) return RnrIoStatus::kTruncated;
  if (magic != kRnrFileMagic) return RnrIoStatus::kMagicMismatch;

  uint32_t fileVersion = 0;
  if (!ReadPod(cursor, fileVersion)) return RnrIoStatus::kTruncated;
  if (fileVersion != kRnrFileVersion) return RnrIoStatus::kVersionMismatch;
  outHeader.fileVersion = fileVersion;

  if (!ReadPod(cursor, outHeader.timestampNanos)) return RnrIoStatus::kTruncated;
  if (!ReadPod(cursor, outHeader.width)) return RnrIoStatus::kTruncated;
  if (!ReadPod(cursor, outHeader.height)) return RnrIoStatus::kTruncated;

  uint32_t rawBackend = 0;
  if (!ReadPod(cursor, rawBackend)) return RnrIoStatus::kTruncated;
  outHeader.backend = static_cast<BackendHint>(rawBackend);

  uint32_t uriLength = 0;
  if (!ReadPod(cursor, uriLength)) return RnrIoStatus::kTruncated;
  if (uriLength > kRnrMaxUriBytes) return RnrIoStatus::kUriTooLong;
  if (cursor.size() < uriLength) return RnrIoStatus::kTruncated;
  outHeader.uri.assign(reinterpret_cast<const char*>(cursor.data()), uriLength);
  cursor = cursor.subspan(uriLength);

  outWireBytes.assign(cursor.begin(), cursor.end());
  return RnrIoStatus::kOk;
}

RnrIoStatus SaveRnrFile(const std::filesystem::path& path, const RnrHeader& header,
                        std::span<const uint8_t> wireBytes) {
  if (header.uri.size() > kRnrMaxUriBytes) return RnrIoStatus::kUriTooLong;

  const auto buffer = EncodeRnrBuffer(header, wireBytes);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return RnrIoStatus::kWriteFailed;
  out.write(reinterpret_cast<const char*>(buffer.data()),
            static_cast<std::streamsize>(buffer.size()));
  if (!out) return RnrIoStatus::kWriteFailed;
  return RnrIoStatus::kOk;
}

RnrIoStatus LoadRnrFile(const std::filesystem::path& path, RnrHeader& outHeader,
                        std::vector<uint8_t>& outWireBytes) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return RnrIoStatus::kReadFailed;
  const auto size = static_cast<std::streamoff>(in.tellg());
  if (size < 0) return RnrIoStatus::kReadFailed;
  in.seekg(0);

  std::vector<uint8_t> buffer(static_cast<std::size_t>(size));
  if (size > 0) {
    in.read(reinterpret_cast<char*>(buffer.data()), size);
    if (!in || in.gcount() != size) return RnrIoStatus::kReadFailed;
  }

  return ParseRnrBuffer(buffer, outHeader, outWireBytes);
}

}  // namespace donner::editor::sandbox
