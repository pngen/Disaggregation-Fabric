// Disaggregation Fabric — framed TCP protocol.
// Frames carry: magic, version, frame type (canonical table), correlation id,
// bounded payload length, and a frame checksum. The decoder handles arbitrary
// partial reads; the encoder permits partial writes. Unknown/oversized/malformed
// frames are rejected before any mutation. The maximum frame length is derived
// from a canonical per-type table — there is no stale hard-coded numeric max.
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <optional>
#include <utility>

namespace dfabric {
namespace proto {

enum class FrameType : std::uint32_t {
  HELLO = 1,
  REGISTER = 2,
  PUBLISH_NODE = 3,
  PUBLISH_RESOURCE = 4,
  HEARTBEAT = 5,
  COMPOSE_REQ = 6,
  RESERVE_REQ = 7,
  COMMIT_REQ = 8,
  ACTIVATE_REQ = 9,
  QUERY_STATE = 10,
  ACK = 11,
  NACK = 12,
  COMPOSE_RESP = 13,
  RESERVE_RESP = 14,
  COMMIT_RESP = 15,
  ACTIVATE_RESP = 16,
  STATE_RESP = 17,
  ERR = 18,
  BYE = 19,
  SAVE_REQ = 20,
  SAVE_RESP = 21,
  RELEASE_REQ = 22,
  RELEASE_RESP = 23,
};

const char* ToString(FrameType t);

// Canonical maximum payload length per frame type. Unknown types have no entry.
std::uint32_t MaxFrameLen(FrameType t);

// Global bounded maximum derived from the canonical table.
std::uint32_t GlobalMaxFrameLen();

// Frame constants.
constexpr std::uint32_t kFrameMagic = 0x31465044u;   // "DFP1"
constexpr std::uint8_t  kFrameVersion = 1;
constexpr std::size_t   kHeaderLen = 4 + 1 + 4 + 8 + 4;

// A fully-decoded frame.
struct Frame {
  FrameType type = FrameType::HELLO;
  std::uint64_t correlation = 0;
  std::vector<std::uint8_t> payload;
};

// Encode a complete frame (header + payload + checksum).
std::vector<std::uint8_t> Encode(FrameType type, std::uint64_t correlation,
                                 const std::uint8_t* payload, std::size_t payload_len);

// Incremental frame decoder. Feed arbitrary chunks; returns decoded frames.
class FrameDecoder {
 public:
  // Feed bytes, appends decoded frames to `out`. Returns false on a protocol
  // violation (bad magic/version/type/oversize/checksum); the stream must be
  // considered dead after a rejection.
  bool Feed(const std::uint8_t* data, std::size_t len, std::vector<Frame>& out, std::string& err);
  bool HasPartial() const { return !buffer_.empty(); }
  std::size_t BufferedBytes() const { return buffer_.size(); }
 private:
  std::vector<std::uint8_t> buffer_;
};

}  // namespace proto
}  // namespace dfabric