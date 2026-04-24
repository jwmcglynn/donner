/// @file
///
/// Unit tests for SessionCodec: round-trip encode/decode, malformed input
/// handling, and the blocking fd helpers.

#include "donner/editor/sandbox/SessionCodec.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstring>

#include "donner/editor/sandbox/SessionProtocol.h"

namespace donner::editor::sandbox {
namespace {

TEST(SessionCodecTest, RoundTripEmptyPayload) {
  SessionFrame frame;
  frame.requestId = 42;
  frame.opcode = SessionOpcode::kHandshake;

  std::vector<uint8_t> encoded = EncodeFrame(frame);
  ASSERT_EQ(encoded.size(), kSessionFrameHeaderSize);

  SessionFrame decoded;
  std::size_t consumed = 0;
  ASSERT_TRUE(DecodeFrame(encoded, decoded, consumed));
  EXPECT_EQ(consumed, kSessionFrameHeaderSize);
  EXPECT_EQ(decoded.requestId, 42u);
  EXPECT_EQ(decoded.opcode, SessionOpcode::kHandshake);
  EXPECT_TRUE(decoded.payload.empty());
}

TEST(SessionCodecTest, RoundTripWithPayload) {
  SessionFrame frame;
  frame.requestId = 123456789;
  frame.opcode = SessionOpcode::kLoadBytes;
  frame.payload = {0x01, 0x02, 0x03, 0xFF, 0xFE, 0xFD};

  std::vector<uint8_t> encoded = EncodeFrame(frame);
  ASSERT_EQ(encoded.size(), kSessionFrameHeaderSize + 6);

  SessionFrame decoded;
  std::size_t consumed = 0;
  ASSERT_TRUE(DecodeFrame(encoded, decoded, consumed));
  EXPECT_EQ(consumed, encoded.size());
  EXPECT_EQ(decoded.requestId, 123456789u);
  EXPECT_EQ(decoded.opcode, SessionOpcode::kLoadBytes);
  EXPECT_EQ(decoded.payload, frame.payload);
}

TEST(SessionCodecTest, RoundTripLargePayload) {
  SessionFrame frame;
  frame.requestId = 1;
  frame.opcode = SessionOpcode::kFrame;
  frame.payload.resize(100000, 0xAB);

  std::vector<uint8_t> encoded = EncodeFrame(frame);
  ASSERT_EQ(encoded.size(), kSessionFrameHeaderSize + 100000);

  SessionFrame decoded;
  std::size_t consumed = 0;
  ASSERT_TRUE(DecodeFrame(encoded, decoded, consumed));
  EXPECT_EQ(consumed, encoded.size());
  EXPECT_EQ(decoded.payload.size(), 100000u);
  EXPECT_EQ(decoded.payload[0], 0xAB);
  EXPECT_EQ(decoded.payload[99999], 0xAB);
}

TEST(SessionCodecTest, RoundTripMultipleFrames) {
  SessionFrame f1;
  f1.requestId = 1;
  f1.opcode = SessionOpcode::kHandshake;

  SessionFrame f2;
  f2.requestId = 2;
  f2.opcode = SessionOpcode::kHandshakeAck;
  f2.payload = {0x01, 0x00, 0x00, 0x00};

  std::vector<uint8_t> encoded1 = EncodeFrame(f1);
  std::vector<uint8_t> encoded2 = EncodeFrame(f2);

  // Concatenate both frames.
  std::vector<uint8_t> combined;
  combined.insert(combined.end(), encoded1.begin(), encoded1.end());
  combined.insert(combined.end(), encoded2.begin(), encoded2.end());

  SessionFrame decoded;
  std::size_t consumed = 0;

  // Decode first frame.
  ASSERT_TRUE(DecodeFrame(combined, decoded, consumed));
  EXPECT_EQ(decoded.requestId, 1u);
  EXPECT_EQ(decoded.opcode, SessionOpcode::kHandshake);

  // Decode second frame from remainder.
  std::span<const uint8_t> remainder(combined.data() + consumed, combined.size() - consumed);
  std::size_t consumed2 = 0;
  ASSERT_TRUE(DecodeFrame(remainder, decoded, consumed2));
  EXPECT_EQ(decoded.requestId, 2u);
  EXPECT_EQ(decoded.opcode, SessionOpcode::kHandshakeAck);
}

TEST(SessionCodecTest, WrongMagicReturnsProtocolError) {
  std::vector<uint8_t> bad(kSessionFrameHeaderSize, 0);
  // Write wrong magic.
  uint32_t wrongMagic = 0xDEADBEEF;
  std::memcpy(bad.data(), &wrongMagic, 4);

  SessionFrame decoded;
  std::size_t consumed = 0;
  EXPECT_FALSE(DecodeFrame(bad, decoded, consumed));
  EXPECT_EQ(consumed, SIZE_MAX);
}

TEST(SessionCodecTest, TruncatedHeaderReturnsNeedMore) {
  std::vector<uint8_t> partial(10, 0);  // Less than kSessionFrameHeaderSize.
  uint32_t magic = kSessionMagic;
  std::memcpy(partial.data(), &magic, 4);

  SessionFrame decoded;
  std::size_t consumed = 0;
  EXPECT_FALSE(DecodeFrame(partial, decoded, consumed));
  EXPECT_EQ(consumed, 0u);
}

TEST(SessionCodecTest, TruncatedPayloadReturnsNeedMore) {
  SessionFrame frame;
  frame.requestId = 1;
  frame.opcode = SessionOpcode::kFrame;
  frame.payload = {1, 2, 3, 4, 5};

  std::vector<uint8_t> encoded = EncodeFrame(frame);
  // Truncate the last byte of the payload.
  encoded.pop_back();

  SessionFrame decoded;
  std::size_t consumed = 0;
  EXPECT_FALSE(DecodeFrame(encoded, decoded, consumed));
  EXPECT_EQ(consumed, 0u);
}

TEST(SessionCodecTest, PayloadExceedsMaxReturnsProtocolError) {
  // Craft a header with a payload length exceeding kSessionMaxPayload.
  std::vector<uint8_t> bad(kSessionFrameHeaderSize, 0);
  uint32_t magic = kSessionMagic;
  std::memcpy(bad.data(), &magic, 4);
  // requestId = 0 (offset 4, 8 bytes)
  // opcode = 1 (offset 12, 4 bytes)
  uint32_t opcode = 1;
  std::memcpy(bad.data() + 12, &opcode, 4);
  // payloadLength = kSessionMaxPayload + 1 (offset 16, 4 bytes)
  uint32_t tooBig = kSessionMaxPayload + 1;
  std::memcpy(bad.data() + 16, &tooBig, 4);

  SessionFrame decoded;
  std::size_t consumed = 0;
  EXPECT_FALSE(DecodeFrame(bad, decoded, consumed));
  EXPECT_EQ(consumed, SIZE_MAX);
}

TEST(SessionCodecTest, ZeroLengthBuffer) {
  std::span<const uint8_t> empty;
  SessionFrame decoded;
  std::size_t consumed = 0;
  EXPECT_FALSE(DecodeFrame(empty, decoded, consumed));
  EXPECT_EQ(consumed, 0u);
}

TEST(SessionCodecTest, RoundTripAllOpcodes) {
  // Test a representative sample of opcodes.
  const SessionOpcode opcodes[] = {
      SessionOpcode::kHandshake,      SessionOpcode::kShutdown,     SessionOpcode::kSetViewport,
      SessionOpcode::kLoadBytes,      SessionOpcode::kPointerEvent, SessionOpcode::kKeyEvent,
      SessionOpcode::kFrame,          SessionOpcode::kHandshakeAck, SessionOpcode::kShutdownAck,
      SessionOpcode::kExportResponse, SessionOpcode::kDiagnostic,   SessionOpcode::kError,
  };

  for (auto op : opcodes) {
    SessionFrame frame;
    frame.requestId = static_cast<uint64_t>(op) * 100;
    frame.opcode = op;
    frame.payload = {0xAA, 0xBB};

    std::vector<uint8_t> encoded = EncodeFrame(frame);
    SessionFrame decoded;
    std::size_t consumed = 0;
    ASSERT_TRUE(DecodeFrame(encoded, decoded, consumed))
        << "Failed for opcode " << static_cast<uint32_t>(op);
    EXPECT_EQ(decoded.opcode, op);
    EXPECT_EQ(decoded.requestId, frame.requestId);
    EXPECT_EQ(decoded.payload, frame.payload);
  }
}

TEST(SessionCodecTest, FdRoundTripViaPipe) {
  int pipeFds[2];
  ASSERT_EQ(::pipe(pipeFds), 0);

  SessionFrame frame;
  frame.requestId = 77;
  frame.opcode = SessionOpcode::kFrame;
  frame.payload = {0x10, 0x20, 0x30};

  std::string writeErr;
  ASSERT_TRUE(WriteFrame(pipeFds[1], frame, writeErr)) << writeErr;
  ::close(pipeFds[1]);

  SessionFrame decoded;
  std::string readErr;
  ASSERT_TRUE(ReadNextFrame(pipeFds[0], decoded, readErr)) << readErr;
  ::close(pipeFds[0]);

  EXPECT_EQ(decoded.requestId, 77u);
  EXPECT_EQ(decoded.opcode, SessionOpcode::kFrame);
  EXPECT_EQ(decoded.payload, frame.payload);
}

TEST(SessionCodecTest, ReadNextFrameEOF) {
  int pipeFds[2];
  ASSERT_EQ(::pipe(pipeFds), 0);
  ::close(pipeFds[1]);  // Close write end immediately → EOF.

  SessionFrame decoded;
  std::string err;
  EXPECT_FALSE(ReadNextFrame(pipeFds[0], decoded, err));
  ::close(pipeFds[0]);
}

}  // namespace
}  // namespace donner::editor::sandbox
