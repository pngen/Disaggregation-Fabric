// Disaggregation Fabric — protocol framing tests.
// Round-trip (including the newest frame type), partial reads, malformed magic,
// bad version, unknown type, oversized frame, checksum mismatch, correlation.
#include "test_util.hpp"
#include "dfabric/protocol.hpp"
#include <vector>
#include <string>

using namespace dfabric::proto;

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);

  // Round-trip every frame type including the newest (RELEASE_RESP = 23).
  for (std::uint32_t v = 1; v <= 23; ++v) {
    FrameType t = static_cast<FrameType>(v);
    std::vector<std::uint8_t> payload = { 0x01, 0x02, 0x03, 0x04 };
    auto frame = Encode(t, 0xdeadbeef, payload.data(), payload.size());
    CHECK(!frame.empty());
    FrameDecoder dec;
    std::vector<Frame> out; std::string err;
    CHECK(dec.Feed(frame.data(), frame.size(), out, err));
    CHECK(out.size() == 1);
    CHECK(out[0].type == t);
    CHECK(out[0].correlation == 0xdeadbeef);
    CHECK(out[0].payload == payload);
  }

  // Partial reads: feed one byte at a time.
  {
    auto frame = Encode(FrameType::HEARTBEAT, 42, nullptr, 0);
    FrameDecoder dec;
    std::vector<Frame> out; std::string err;
    for (auto b : frame) { CHECK(dec.Feed(&b, 1, out, err)); }
    CHECK(out.size() == 1);
    CHECK(out[0].type == FrameType::HEARTBEAT);
    CHECK(out[0].correlation == 42);
  }

  // Two frames in one chunk.
  {
    auto f1 = Encode(FrameType::ACK, 1, nullptr, 0);
    auto f2 = Encode(FrameType::NACK, 2, nullptr, 0);
    std::vector<std::uint8_t> both = f1; both.insert(both.end(), f2.begin(), f2.end());
    FrameDecoder dec; std::vector<Frame> out; std::string err;
    CHECK(dec.Feed(both.data(), both.size(), out, err));
    CHECK(out.size() == 2);
    CHECK(out[0].type == FrameType::ACK && out[1].type == FrameType::NACK);
  }

  // Malformed magic.
  {
    auto frame = Encode(FrameType::BYE, 7, nullptr, 0);
    frame[0] ^= 0xff;
    FrameDecoder dec; std::vector<Frame> out; std::string err;
    CHECK(!dec.Feed(frame.data(), frame.size(), out, err));
    CHECK(err.find("magic") != std::string::npos);
  }

  // Bad version.
  {
    auto frame = Encode(FrameType::BYE, 7, nullptr, 0);
    frame[4] = 99;
    FrameDecoder dec; std::vector<Frame> out; std::string err;
    CHECK(!dec.Feed(frame.data(), frame.size(), out, err));
    CHECK(err.find("version") != std::string::npos);
  }

  // Unknown frame type (value 999).
  {
    auto frame = Encode(FrameType::BYE, 7, nullptr, 0);
    frame[5] = 0xe7; frame[6] = 0x03; frame[7] = 0x00; frame[8] = 0x00;  // type = 999
    FrameDecoder dec; std::vector<Frame> out; std::string err;
    CHECK(!dec.Feed(frame.data(), frame.size(), out, err));
    CHECK(err.find("unknown") != std::string::npos);
  }

  // Oversized frame (declare payload length far above the type bound).
  {
    auto frame = Encode(FrameType::HEARTBEAT, 7, nullptr, 0);
    // Heartbeat bound is 256; set payload len field (offset 17..20) to 0xFFFFFFFF.
    for (int i = 0; i < 4; ++i) frame[17 + i] = 0xff;
    FrameDecoder dec; std::vector<Frame> out; std::string err;
    CHECK(!dec.Feed(frame.data(), frame.size(), out, err));
    CHECK(err.find("oversized") != std::string::npos);
  }

  // Checksum mismatch (corrupt a payload byte; header still parses).
  {
    std::vector<std::uint8_t> payload = { 1, 2, 3, 4 };
    auto f = Encode(FrameType::COMPOSE_REQ, 5, payload.data(), payload.size());
    f[kHeaderLen] ^= 0xff;   // corrupt the first payload byte
    FrameDecoder dec; std::vector<Frame> out; std::string err;
    CHECK(!dec.Feed(f.data(), f.size(), out, err));
    CHECK(err.find("checksum") != std::string::npos);
  }

  // Partial leftover (clean EOF with an incomplete frame).
  {
    auto frame = Encode(FrameType::BYE, 7, nullptr, 0);
    FrameDecoder dec; std::vector<Frame> out; std::string err;
    CHECK(dec.Feed(frame.data(), frame.size() - 3, out, err));  // incomplete
    CHECK(out.empty());
    CHECK(dec.HasPartial());
  }

  // Oversized send is rejected by Encode itself (returns empty).
  {
    std::vector<std::uint8_t> big(1u << 20);
    auto frame = Encode(FrameType::HEARTBEAT, 1, big.data(), big.size());
    CHECK(frame.empty());
  }

  // Newest frame type has a non-zero canonical bound.
  CHECK(MaxFrameLen(FrameType::RELEASE_RESP) > 0);
  CHECK(GlobalMaxFrameLen() >= MaxFrameLen(FrameType::RELEASE_RESP));

  printf("protocol test ok\n");
  return 0;
}