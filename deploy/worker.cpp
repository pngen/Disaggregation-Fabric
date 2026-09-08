// Disaggregation Fabric — reference worker process.
// Connects to a coordinator, registers, receives its node/resource descriptors
// from the coordinator topology, re-publishes them with its own worker-boot
// ownership over a real framed TCP channel, and heartbeats. A hard OS kill
// (TerminateProcess) must be observable by the coordinator as a disconnect.
#include "dfabric/protocol.hpp"
#include "dfabric/socket.hpp"
#include "dfabric/codec.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <chrono>
#include <string>

using namespace dfabric;
using namespace dfabric::proto;
using namespace dfabric::net;

namespace {
std::vector<std::uint8_t> MakeCorrPayload(std::uint64_t v) {
  WireWriter w; w.U64(v); return w.Take();
}
std::uint64_t ReadCorrPayload(const std::vector<std::uint8_t>& p) {
  WireReader r(p.data(), p.size());
  std::uint64_t v = r.U64();
  return v;
}
bool ReadBool(const std::vector<std::uint8_t>& p) {
  WireReader r(p.data(), p.size());
  return r.U8() != 0;
}
}

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  int port = 0;
  std::uint64_t boot = 0;
  std::string nodesArg;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto val = [&](const char* key) -> std::string { std::string p = a; size_t pos = p.find(key); if (pos != std::string::npos) return p.substr(p.find('=') + 1); return std::string(); };
    if (a.rfind("--coord=", 0) == 0) { size_t e = a.find(':'); host = a.substr(8, e - 8); port = std::atoi(a.substr(e + 1).c_str()); }
    if (a.rfind("--boot=", 0) == 0) boot = std::strtoull(a.substr(7).c_str(), nullptr, 10);
    if (a.rfind("--nodes=", 0) == 0) nodesArg = a.substr(8);
  }
  if (port <= 0) { std::fprintf(stderr, "worker: --coord required\n"); return 1; }

  TcpSocket sock;
  try { sock = TcpSocket::Connect(host, port); }
  catch (const std::exception& e) { std::fprintf(stderr, "worker: connect failed: %s\n", e.what()); return 1; }

  FrameDecoder dec;
  // HELLO(boot)
  auto hello = Encode(FrameType::HELLO, 1, nullptr, 0); (void)hello;
  std::vector<std::uint8_t> body = MakeCorrPayload(boot);
  auto hf = Encode(FrameType::HELLO, 1, body.data(), body.size());
  // Send all (blocking-ish loop).
  for (size_t off = 0; off < hf.size(); ) { long r = sock.SendSome(hf.data() + off, static_cast<long>(hf.size() - off)); if (r < 0) return 1; if (r > 0) off += r; }

  // REGISTER(nodes)
  std::vector<std::uint64_t> nodeIds;
  if (!nodesArg.empty()) {
    std::string cur; for (char c : nodesArg) { if (c == ',') { if (!cur.empty()) nodeIds.push_back(std::strtoull(cur.c_str(), nullptr, 10)); cur.clear(); } else cur += c; }
    if (!cur.empty()) nodeIds.push_back(std::strtoull(cur.c_str(), nullptr, 10));
  }
  WireWriter w; w.U32(static_cast<std::uint32_t>(nodeIds.size()));
  for (auto n : nodeIds) w.U64(n);
  auto regbody = w.Take();
  auto rf = Encode(FrameType::REGISTER, 2, regbody.data(), regbody.size());
  for (size_t off = 0; off < rf.size(); ) { long r = sock.SendSome(rf.data() + off, static_cast<long>(rf.size() - off)); if (r < 0) return 1; if (r > 0) off += r; }

  std::uint64_t corrNext = 100;
  bool alive = true;
  while (alive) {
    std::uint8_t buf[4096];
    long got = sock.RecvSome(buf, 4096);
    if (got == -2) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
    if (got == 0) { break; }  // clean EOF
    if (got < 0) { break; }
    std::vector<Frame> frames; std::string err;
    if (!dec.Feed(buf, static_cast<std::size_t>(got), frames, err)) { std::fprintf(stderr, "worker: protocol error: %s\n", err.c_str()); break; }
    for (auto& f : frames) {
      if (f.type == FrameType::PUBLISH_NODE) {
        Node n; WireReader rd(f.payload.data(), f.payload.size());
        if (DecodeNode(rd, n)) {
          // Stamp dynamic worker ownership.
          n.worker_owner = std::to_string(boot);
          n.worker_boot = WorkerBootId(boot);
          WireWriter out; EncodeNode(out, n); auto b = out.Take();
          auto fr = Encode(FrameType::PUBLISH_NODE, ++corrNext, b.data(), b.size());
          for (size_t off = 0; off < fr.size(); ) { long sr = sock.SendSome(fr.data() + off, (long)(fr.size() - off)); if (sr < 0) { alive = false; break; } if (sr > 0) off += sr; }
        }
      } else if (f.type == FrameType::PUBLISH_RESOURCE) {
        Resource res; WireReader rd(f.payload.data(), f.payload.size());
        if (DecodeResource(rd, res)) {
          res.evidence.owner = std::to_string(boot);
          res.evidence.provenance = "worker-" + std::to_string(boot);
          res.evidence.durable = false;
          res.evidence.state = EvidenceState::FRESH;
          res.evidence.max_age_ms = 3600000;
          res.evidence.generation = EvidenceGeneration(boot + 1);
          res.authority_owner = std::to_string(boot);
          res.health = ResourceHealth::HEALTHY; res.readiness = Readiness::READY; res.reachability = Reachability::REACHABLE;
          WireWriter out; EncodeResource(out, res); auto b = out.Take();
          auto fr = Encode(FrameType::PUBLISH_RESOURCE, ++corrNext, b.data(), b.size());
          for (size_t off = 0; off < fr.size(); ) { long sr = sock.SendSome(fr.data() + off, (long)(fr.size() - off)); if (sr < 0) { alive = false; break; } if (sr > 0) off += sr; }
        }
      } else if (f.type == FrameType::HEARTBEAT) {
        auto hbf = Encode(FrameType::HEARTBEAT, ++corrNext, nullptr, 0);
        for (size_t off = 0; off < hbf.size(); ) { long sr = sock.SendSome(hbf.data() + off, (long)(hbf.size() - off)); if (sr < 0) { alive = false; break; } if (sr > 0) off += sr; }
      }
    }
  }
  sock.Close();
  return 0;
}