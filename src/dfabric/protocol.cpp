// Disaggregation Fabric — framed TCP protocol implementation.
#include "dfabric/protocol.hpp"
#include <cstring>

namespace dfabric {
namespace proto {

const char* ToString(FrameType t) {
  switch (t) {
    case FrameType::HELLO: return "HELLO";
    case FrameType::REGISTER: return "REGISTER";
    case FrameType::PUBLISH_NODE: return "PUBLISH_NODE";
    case FrameType::PUBLISH_RESOURCE: return "PUBLISH_RESOURCE";
    case FrameType::HEARTBEAT: return "HEARTBEAT";
    case FrameType::COMPOSE_REQ: return "COMPOSE_REQ";
    case FrameType::RESERVE_REQ: return "RESERVE_REQ";
    case FrameType::COMMIT_REQ: return "COMMIT_REQ";
    case FrameType::ACTIVATE_REQ: return "ACTIVATE_REQ";
    case FrameType::QUERY_STATE: return "QUERY_STATE";
    case FrameType::ACK: return "ACK";
    case FrameType::NACK: return "NACK";
    case FrameType::COMPOSE_RESP: return "COMPOSE_RESP";
    case FrameType::RESERVE_RESP: return "RESERVE_RESP";
    case FrameType::COMMIT_RESP: return "COMMIT_RESP";
    case FrameType::ACTIVATE_RESP: return "ACTIVATE_RESP";
    case FrameType::STATE_RESP: return "STATE_RESP";
    case FrameType::ERR: return "ERROR";
    case FrameType::BYE: return "BYE";
    case FrameType::SAVE_REQ: return "SAVE_REQ";
    case FrameType::SAVE_RESP: return "SAVE_RESP";
    case FrameType::RELEASE_REQ: return "RELEASE_REQ";
    case FrameType::RELEASE_RESP: return "RELEASE_RESP";
  }
  return "UNKNOWN";
}

std::uint32_t MaxFrameLen(FrameType t) {
  switch (t) {
    case FrameType::HELLO: return 4096;
    case FrameType::REGISTER: return 4096;
    case FrameType::PUBLISH_NODE: return 65536;
    case FrameType::PUBLISH_RESOURCE: return 65536;
    case FrameType::HEARTBEAT: return 256;
    case FrameType::COMPOSE_REQ: return 8192;
    case FrameType::RESERVE_REQ: return 262144;
    case FrameType::COMMIT_REQ: return 8192;
    case FrameType::ACTIVATE_REQ: return 8192;
    case FrameType::QUERY_STATE: return 1024;
    case FrameType::ACK: return 1024;
    case FrameType::NACK: return 4096;
    case FrameType::COMPOSE_RESP: return 524288;
    case FrameType::RESERVE_RESP: return 524288;
    case FrameType::COMMIT_RESP: return 524288;
    case FrameType::ACTIVATE_RESP: return 524288;
    case FrameType::STATE_RESP: return 524288;
    case FrameType::ERR: return 4096;
    case FrameType::BYE: return 256;
    case FrameType::SAVE_REQ: return 1024;
    case FrameType::SAVE_RESP: return 4096;
    case FrameType::RELEASE_REQ: return 1024;
    case FrameType::RELEASE_RESP: return 1024;
  }
  return 0;  // unknown type: not addressable
}

std::uint32_t GlobalMaxFrameLen() {
  std::uint32_t m = 0;
  for (std::uint32_t v = 1; v <= 23; ++v) {
    std::uint32_t x = MaxFrameLen(static_cast<FrameType>(v));
    if (x > m) m = x;
  }
  return m;
}

namespace {
std::uint64_t Fnv(const std::uint8_t* d, std::size_t n) {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (std::size_t i = 0; i < n; ++i) { h ^= static_cast<std::uint64_t>(d[i]); h *= 0x100000001b3ULL; }
  return h;
}
void PutU32(std::vector<std::uint8_t>& v, std::uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 0xff); }
void PutU64(std::vector<std::uint8_t>& v, std::uint64_t x) { for (int i = 0; i < 8; ++i) v.push_back((x >> (8 * i)) & 0xff); }
std::uint32_t GetU32(const std::uint8_t* d, std::size_t off) {
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(d[off + i]) << (8 * i);
  return v;
}
std::uint64_t GetU64(const std::uint8_t* d, std::size_t off) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(d[off + i]) << (8 * i);
  return v;
}
}  // namespace

std::vector<std::uint8_t> Encode(FrameType type, std::uint64_t correlation,
                                 const std::uint8_t* payload, std::size_t payload_len) {
  std::uint32_t max = MaxFrameLen(type);
  if (max == 0 || payload_len > max) return {};   // oversized / unknown type
  std::vector<std::uint8_t> v;
  PutU32(v, kFrameMagic);
  v.push_back(kFrameVersion);
  PutU32(v, static_cast<std::uint32_t>(type));
  PutU64(v, correlation);
  PutU32(v, static_cast<std::uint32_t>(payload_len));
  if (payload && payload_len) v.insert(v.end(), payload, payload + payload_len);
  std::uint64_t sum = Fnv(v.data(), v.size());
  PutU64(v, sum);
  return v;
}

bool FrameDecoder::Feed(const std::uint8_t* data, std::size_t len, std::vector<Frame>& out, std::string& err) {
  const std::uint32_t gmax = GlobalMaxFrameLen();
  buffer_.insert(buffer_.end(), data, data + len);
  for (;;) {
    if (buffer_.size() < kHeaderLen) return true;
    std::uint32_t magic = GetU32(buffer_.data(), 0);
    if (magic != kFrameMagic) { err = "bad frame magic"; return false; }
    std::uint8_t ver = buffer_[4];
    if (ver != kFrameVersion) { err = "unsupported frame version"; return false; }
    std::uint32_t typeRaw = GetU32(buffer_.data(), 5);
    FrameType type = static_cast<FrameType>(typeRaw);
    std::uint32_t max = MaxFrameLen(type);
    if (max == 0) { err = "unknown frame type"; return false; }
    std::uint32_t payLen = GetU32(buffer_.data(), 17);
    if (payLen > max || payLen > gmax) { err = "oversized frame"; return false; }
    std::size_t total = kHeaderLen + static_cast<std::size_t>(payLen) + 8;  // header + payload + checksum
    if (buffer_.size() < total) return true;   // need more bytes (partial read)
    std::uint64_t stored = GetU64(buffer_.data(), kHeaderLen + payLen);
    std::uint64_t computed = Fnv(buffer_.data(), kHeaderLen + payLen);
    if (stored != computed) { err = "frame checksum mismatch"; return false; }
    Frame f;
    f.type = type;
    f.correlation = GetU64(buffer_.data(), 9);
    if (payLen) f.payload.assign(buffer_.begin() + kHeaderLen, buffer_.begin() + kHeaderLen + payLen);
    out.push_back(std::move(f));
    buffer_.erase(buffer_.begin(), buffer_.begin() + total);
  }
}

}  // namespace proto
}  // namespace dfabric