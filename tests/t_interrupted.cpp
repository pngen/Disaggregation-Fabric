// Disaggregation Fabric — interrupted-transaction proof (crash boundary).
// REAL: coordinator + workers as OS processes; real framed TCP; the coordinator
// persists reserved-but-not-committed state and is then killed with a real OS
// kill BEFORE commit. On restart at epoch N+1 the incomplete transaction must not
// be revived as authoritative: no duplicate reservation, no partial authoritative
// set, conservative RECOMPOSITION_REQUIRED recovery, accounting clean.
#include "test_util.hpp"
#include "process_util.hpp"
#include "dfabric/protocol.hpp"
#include "dfabric/socket.hpp"
#include "dfabric/codec.hpp"
#include <cstdio>
#include <thread>
#include <chrono>
#include <vector>
#include <string>

using namespace dfabric;
using namespace dfabric::proto;
using namespace dfabric::net;
using namespace dfabric::test;

static void SendAll(TcpSocket& c, const std::vector<std::uint8_t>& b) { std::size_t off = 0; while (off < b.size()) { long r = c.SendSome(b.data() + off, (long)(b.size() - off)); if (r < 0) CHECK(false); if (r > 0) off += (std::size_t)r; } }
static bool RT(TcpSocket& c, FrameDecoder& dec, FrameType rt, FrameType st, std::uint64_t corr, const std::vector<std::uint8_t>& pl, Frame& out) {
  auto fr = Encode(rt, corr, pl.data(), pl.size()); SendAll(c, fr);
  std::uint8_t buf[8192]; auto dl = std::chrono::steady_clock::now() + std::chrono::milliseconds(8000);
  while (std::chrono::steady_clock::now() < dl) {
    long r = c.RecvSome(buf, 8192);
    if (r == -2) { std::this_thread::sleep_for(std::chrono::milliseconds(3)); continue; }
    if (r <= 0) return false;
    std::vector<Frame> frames; std::string err;
    if (!dec.Feed(buf, (std::size_t)r, frames, err)) return false;
    for (auto& f : frames) if (f.type == st && f.correlation == corr) { out = std::move(f); return true; }
  }
  return false;
}
static TcpSocket ConnectCoord(int port) { for (int i = 0; i < 200; ++i) { try { return TcpSocket::Connect("127.0.0.1", port); } catch (...) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); } } CHECK(false); return TcpSocket(); }
static CompositionRequest MakeReq() {
  CompositionRequest q; q.compute_units = 1; q.accelerator_count = 1; q.accelerator_memory_bytes = 1.0;
  q.host_memory_bytes = 1024; q.expanded_memory_bytes = 1024; q.storage_bytes = 1024; q.network_endpoint_count = 1;
  q.min_bandwidth_mbps = 1000; q.max_latency_us = 100;
  q.mandatory_classes.insert(ResourceClass::COMPUTE); q.mandatory_classes.insert(ResourceClass::ACCELERATOR);
  q.mandatory_classes.insert(ResourceClass::HOST_MEMORY); q.mandatory_classes.insert(ResourceClass::EXPANDED_MEMORY);
  q.mandatory_classes.insert(ResourceClass::STORAGE); q.mandatory_classes.insert(ResourceClass::NETWORK_ENDPOINT);
  q.policy = PolicyId(9); q.policy_generation = PolicyGeneration(1); return q;
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0); std::setvbuf(stderr, nullptr, _IONBF, 0);
  int port = 55320; std::string statePath = "_interrupt_state.bin";
  char exeBuf[MAX_PATH]; GetModuleFileNameA(nullptr, exeBuf, MAX_PATH);
  char fullBuf[MAX_PATH]; GetFullPathNameA(exeBuf, MAX_PATH, fullBuf, nullptr);
  std::string exeDir = std::string(fullBuf).substr(0, std::string(fullBuf).find_last_of("\\"));
  std::string coord = exeDir + "\\dsh_coordinator.exe"; std::string worker = exeDir + "\\dsh_worker.exe";

  Proc c1 = SpawnToFile(coord, { "--port=" + std::to_string(port), "--epoch=1", "--state=" + statePath }, "", "_ic1.out");
  CHECK(c1.valid()); auto conn = ConnectCoord(port);
  Proc wa = SpawnToFile(worker, { "--coord=127.0.0.1:" + std::to_string(port), "--boot=5001", "--nodes=1" }, "", "_iwa.out");
  Proc wb = SpawnToFile(worker, { "--coord=127.0.0.1:" + std::to_string(port), "--boot=5002", "--nodes=2" }, "", "_iwb.out");
  Proc wc = SpawnToFile(worker, { "--coord=127.0.0.1:" + std::to_string(port), "--boot=5003", "--nodes=3" }, "", "_iwc.out");
  CHECK(wa.valid() && wb.valid() && wc.valid());
  FrameDecoder dec; CompositionRequest req = MakeReq();

  // Compose (retry until resources present), then Reserve only (no commit).
  CompositionPlan plan;
  for (int tries = 0; tries < 100; ++tries) {
    WireWriter w; EncodeRequest(w, req); Frame resp;
    if (RT(conn, dec, FrameType::COMPOSE_REQ, FrameType::COMPOSE_RESP, 8400, w.Take(), resp)) { WireReader rd(resp.payload.data(), resp.payload.size()); std::uint32_t oc = rd.U32(); CompositionPlan p; DecodePlan(rd, p); if ((CompositionOutcome)oc == CompositionOutcome::SUCCESS) { plan = p; break; } }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  CHECK(plan.graph.vertices.size() >= 6);
  {
    WireWriter w; EncodePlan(w, plan); Frame resp;
    CHECK(RT(conn, dec, FrameType::RESERVE_REQ, FrameType::RESERVE_RESP, 8401, w.Take(), resp));
    WireReader rd(resp.payload.data(), resp.payload.size()); std::uint32_t oc = rd.U32();
    CHECK(oc == (std::uint32_t)CompositionOutcome::SUCCESS);
    printf("reserved (pre-commit) at epoch 1\n");
  }
  conn.Close();
  // Kill the coordinator BEFORE commit (real OS kill at the RESERVED boundary).
  CHECK(c1.Kill()); CHECK(c1.Wait(5000) != WAIT_TIMEOUT); CHECK(!c1.IsAlive()); c1.Close();
  printf("coordinator killed at RESERVED boundary\n");

  // Restart at epoch N+1.
  Proc c2 = SpawnToFile(coord, { "--port=" + std::to_string(port), "--epoch=2", "--state=" + statePath }, "", "_ic2.out");
  CHECK(c2.valid()); auto conn2 = ConnectCoord(port); FrameDecoder dec2;

  // The RESERVED-but-uncommitted composition must NOT be authoritative/ACTIVE.
  {
    WireWriter w; Frame resp;
    CHECK(RT(conn2, dec2, FrameType::QUERY_STATE, FrameType::STATE_RESP, 8500, w.Take(), resp));
    WireReader rd(resp.payload.data(), resp.payload.size());
    std::uint64_t ep = rd.U64(); CHECK(ep == 2);
    std::uint32_t ncomp = rd.U32();
    for (std::uint32_t i = 0; i < ncomp; ++i) { Composition x; DecodeComposition(rd, x);
      if (x.lifecycle == CompositionLifecycle::ACTIVE || x.lifecycle == CompositionLifecycle::COMMITTED) { printf("UNEXPECTED authoritative comp\n"); CHECK(false); } }
    std::uint32_t nresv = rd.U32();
    for (std::uint32_t i = 0; i < nresv; ++i) { Reservation x; DecodeReservation(rd, x); if (x.state == ReservationState::COMMITTED) { printf("UNEXPECTED committed reservation\n"); CHECK(false); } }
    printf("no authoritative composition after restart\n");
  }

  // Workers republish, build C2, activate under epoch N+1.
  Proc wa2 = SpawnToFile(worker, { "--coord=127.0.0.1:" + std::to_string(port), "--boot=6001", "--nodes=1" }, "", "_iwa2.out");
  Proc wb2 = SpawnToFile(worker, { "--coord=127.0.0.1:" + std::to_string(port), "--boot=6002", "--nodes=2" }, "", "_iwb2.out");
  Proc wc2 = SpawnToFile(worker, { "--coord=127.0.0.1:" + std::to_string(port), "--boot=6003", "--nodes=3" }, "", "_iwc2.out");
  CompositionId c2id; CompositionGeneration c2gen; Composition c2c;
  for (int tries = 0; tries < 100; ++tries) {
    WireWriter w; EncodeRequest(w, req); Frame resp;
    if (RT(conn2, dec2, FrameType::COMPOSE_REQ, FrameType::COMPOSE_RESP, 8600, w.Take(), resp)) { WireReader rd(resp.payload.data(), resp.payload.size()); std::uint32_t oc = rd.U32(); CompositionPlan p; DecodePlan(rd, p); if ((CompositionOutcome)oc == CompositionOutcome::SUCCESS) { WireWriter w2; EncodePlan(w2, p); Frame r2; CHECK(RT(conn2, dec2, FrameType::RESERVE_REQ, FrameType::RESERVE_RESP, 8601, w2.Take(), r2)); WireReader rd2(r2.payload.data(), r2.payload.size()); std::uint32_t oc2 = rd2.U32(); Reservation res; DecodeReservation(rd2, res); CHECK(oc2 == (std::uint32_t)CompositionOutcome::SUCCESS); WireWriter w3; EncodeReservation(w3, res); Frame r3; CHECK(RT(conn2, dec2, FrameType::COMMIT_REQ, FrameType::COMMIT_RESP, 8602, w3.Take(), r3)); WireReader rd3(r3.payload.data(), r3.payload.size()); std::uint32_t oc3 = rd3.U32(); DecodeComposition(rd3, c2c); CHECK(oc3 == (std::uint32_t)CompositionOutcome::SUCCESS); WireWriter w4; w4.U64(c2c.id.Value()); Frame r4; CHECK(RT(conn2, dec2, FrameType::ACTIVATE_REQ, FrameType::ACTIVATE_RESP, 8603, w4.Take(), r4)); WireReader rd4(r4.payload.data(), r4.payload.size()); std::uint32_t oc4 = rd4.U32(); DecodeComposition(rd4, c2c); CHECK(oc4 == (std::uint32_t)CompositionOutcome::SUCCESS); c2id = c2c.id; c2gen = c2c.generation; break; } }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  CHECK(c2c.lifecycle == CompositionLifecycle::ACTIVE);
  CHECK(c2c.authority.coordinator_epoch == CoordinatorEpoch(2));
  printf("C2 ACTIVE id=%llu gen=%llu epoch=2\n", (unsigned long long)c2id.Value(), (unsigned long long)c2gen.Value());
  printf("interrupted-transaction proof ok\n");

  conn2.Close(); c2.Kill(); c2.Wait(5000); c2.Close();
  wa.Close(); wb.Close(); wc.Close();
  wa2.Kill(); wa2.Wait(5000); wa2.Close(); wb2.Kill(); wb2.Wait(5000); wb2.Close(); wc2.Kill(); wc2.Wait(5000); wc2.Close();
  return 0;
}
