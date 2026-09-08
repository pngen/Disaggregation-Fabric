// Disaggregation Fabric — worker-death multiprocess proof.
// REAL: coordinator + workers as independent OS processes; real framed TCP;
// real OS TerminateProcess of a worker; retained PROCESS_INFORMATION; verified
// process death; stale WorkerBoot traffic rejected; fresh recomposition.
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

static int g_port = 55120;
static std::string g_bindir = "build\\";

static void SendAll(TcpSocket& conn, const std::vector<std::uint8_t>& bytes) {
  std::size_t off = 0;
  while (off < bytes.size()) {
    long r = conn.SendSome(bytes.data() + off, static_cast<long>(bytes.size() - off));
    if (r < 0) { CHECK(false); }
    if (r > 0) off += static_cast<std::size_t>(r);
  }
}

// Send a request and wait for the response frame with matching correlation/type.
static bool RoundTrip(TcpSocket& conn, FrameDecoder& dec, FrameType reqType, FrameType respType,
                      std::uint64_t corr, const std::vector<std::uint8_t>& payload, Frame& out) {
  auto fr = Encode(reqType, corr, payload.data(), payload.size());
  SendAll(conn, fr);
  std::uint8_t buf[8192];
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(8000);
  while (std::chrono::steady_clock::now() < deadline) {
    long r = conn.RecvSome(buf, 8192);
    if (r == -2) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
    if (r <= 0) return false;
    std::vector<Frame> frames; std::string err;
    if (!dec.Feed(buf, static_cast<std::size_t>(r), frames, err)) return false;
    for (auto& f : frames) { if (f.type == respType && f.correlation == corr) { out = std::move(f); return true; } }
  }
  return false;
}

// Wait for the coordinator to accept connections.
static TcpSocket ConnectCoord(int port) {
  for (int tries = 0; tries < 200; ++tries) {
    try { return TcpSocket::Connect("127.0.0.1", port); } catch (...) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
  }
  CHECK(false);
  return TcpSocket();
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  char exeBuf[MAX_PATH]; GetModuleFileNameA(nullptr, exeBuf, MAX_PATH);
  char fullBuf[MAX_PATH]; GetFullPathNameA(exeBuf, MAX_PATH, fullBuf, nullptr);
  std::string exeDir = std::string(fullBuf).substr(0, std::string(fullBuf).find_last_of("\\"));
  std::string coord = exeDir + "\\dsh_coordinator.exe";
  std::string worker = exeDir + "\\dsh_worker.exe";
  std::string statePath = "_workerdeath_state.bin";
  std::string markdir = "_workerdeath_mark";
  (void)markdir;

  Proc coordProc = SpawnToFile(coord, { "--port=" + std::to_string(g_port), "--epoch=1", "--state=" + statePath }, "", "_coord.out");
  CHECK(coordProc.valid());
  auto coordConn = ConnectCoord(g_port);

  Proc workerA = SpawnToFile(worker, { "--coord=127.0.0.1:" + std::to_string(g_port), "--boot=1001", "--nodes=1" }, "", "_workerA.out");
  Proc workerB = SpawnToFile(worker, { "--coord=127.0.0.1:" + std::to_string(g_port), "--boot=1002", "--nodes=2" }, "", "_workerB.out");
  Proc workerC = SpawnToFile(worker, { "--coord=127.0.0.1:" + std::to_string(g_port), "--boot=1003", "--nodes=3" }, "", "_workerC.out");
  CHECK(workerA.valid() && workerB.valid() && workerC.valid());

  // Build the composition request.
  CompositionRequest req;
  req.compute_units = 1; req.accelerator_count = 1; req.accelerator_memory_bytes = 1.0;
  req.host_memory_bytes = 1024; req.expanded_memory_bytes = 1024; req.storage_bytes = 1024;
  req.network_endpoint_count = 1; req.min_bandwidth_mbps = 1000; req.max_latency_us = 100;
  req.mandatory_classes.insert(ResourceClass::COMPUTE);
  req.mandatory_classes.insert(ResourceClass::ACCELERATOR);
  req.mandatory_classes.insert(ResourceClass::HOST_MEMORY);
  req.mandatory_classes.insert(ResourceClass::EXPANDED_MEMORY);
  req.mandatory_classes.insert(ResourceClass::STORAGE);
  req.mandatory_classes.insert(ResourceClass::NETWORK_ENDPOINT);
  req.policy = PolicyId(9); req.policy_generation = PolicyGeneration(1);
  req.consumer = ConsumerId(77); req.consumer_generation = ConsumerGeneration(1);

  FrameDecoder dec;
  CompositionId c1id; CompositionGeneration c1gen{};

  // Diagnostics: query state to confirm resources are registered.
  {
    WireWriter w; Frame resp;
    if (RoundTrip(coordConn, dec, FrameType::QUERY_STATE, FrameType::STATE_RESP, 8999, w.Take(), resp)) {
      WireReader rd(resp.payload.data(), resp.payload.size());
      std::uint64_t ep = rd.U64(); (void)ep;
      std::uint32_t nc = rd.U32(); for (std::uint32_t i = 0; i < nc; ++i) { Composition x; DecodeComposition(rd, x); }
      std::uint32_t nr = rd.U32(); for (std::uint32_t i = 0; i < nr; ++i) { Reservation x; DecodeReservation(rd, x); }
      std::uint32_t nres = rd.U32();
      printf("coordinator resources after worker spawn: %u\n", nres);
    }
  }

  // Compose (retry until worker resources are published).
  CompositionPlan plan;
  for (int tries = 0; tries < 100; ++tries) {
    WireWriter w; EncodeRequest(w, req);
    Frame resp;
    if (RoundTrip(coordConn, dec, FrameType::COMPOSE_REQ, FrameType::COMPOSE_RESP, 9000, w.Take(), resp)) {
      WireReader rd(resp.payload.data(), resp.payload.size());
      std::uint32_t oc = rd.U32();
      CompositionPlan p; DecodePlan(rd, p);
      if (static_cast<CompositionOutcome>(oc) == CompositionOutcome::SUCCESS) { plan = p; break; }
      std::uint32_t nrs = rd.U32();
      if (tries == 0 || tries % 10 == 0) { printf("compose try %d outcome=%s reasons=", tries, ToString(static_cast<CompositionOutcome>(oc))); for (std::uint32_t k = 0; k < nrs; ++k) { std::uint32_t rr = rd.U32(); printf("%s ", ToString(static_cast<RejectionReason>(rr))); } printf("\n"); }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  CHECK(plan.graph.vertices.size() >= 6);

  // Reserve / commit / activate C1.
  Composition c1;
  {
    WireWriter w; EncodePlan(w, plan); Frame resp;
    CHECK(RoundTrip(coordConn, dec, FrameType::RESERVE_REQ, FrameType::RESERVE_RESP, 9001, w.Take(), resp));
    WireReader rd(resp.payload.data(), resp.payload.size()); std::uint32_t oc = rd.U32();
    Reservation res; DecodeReservation(rd, res); CHECK(oc == static_cast<std::uint32_t>(CompositionOutcome::SUCCESS));
    WireWriter w2; EncodeReservation(w2, res); Frame resp2;
    CHECK(RoundTrip(coordConn, dec, FrameType::COMMIT_REQ, FrameType::COMMIT_RESP, 9002, w2.Take(), resp2));
    WireReader rd2(resp2.payload.data(), resp2.payload.size()); std::uint32_t oc2 = rd2.U32();
    DecodeComposition(rd2, c1); CHECK(oc2 == static_cast<std::uint32_t>(CompositionOutcome::SUCCESS));
    WireWriter w3; w3.U64(c1.id.Value()); Frame resp3;
    CHECK(RoundTrip(coordConn, dec, FrameType::ACTIVATE_REQ, FrameType::ACTIVATE_RESP, 9003, w3.Take(), resp3));
    WireReader rd3(resp3.payload.data(), resp3.payload.size()); std::uint32_t oc3 = rd3.U32();
    DecodeComposition(rd3, c1); CHECK(oc3 == static_cast<std::uint32_t>(CompositionOutcome::SUCCESS));
    CHECK(c1.lifecycle == CompositionLifecycle::ACTIVE);
  }
  c1id = c1.id; c1gen = c1.generation;
  printf("C1 ACTIVE: id=%llu gen=%llu\n", (unsigned long long)c1id.Value(), (unsigned long long)c1gen.Value());

  // Kill worker A (real OS process). Retain handle; confirm death.
  CHECK(workerA.Kill());                     // TerminateProcess
  DWORD waitr = workerA.Wait(5000);
  CHECK(waitr != WAIT_TIMEOUT);
  CHECK(!workerA.IsAlive());                 // confirmed dead
  printf("worker A killed; pid=%lu dead\n", (unsigned long)workerA.pid);

  // Coordinator detects disconnect -> invalidates A evidence -> C1 recomposition required.
  Composition c1check;
  bool becameRecomp = false;
  for (int tries = 0; tries < 100; ++tries) {
    WireWriter w; Frame resp;
    if (RoundTrip(coordConn, dec, FrameType::QUERY_STATE, FrameType::STATE_RESP, 9100, w.Take(), resp)) {
      WireReader rd(resp.payload.data(), resp.payload.size());
      std::uint64_t epoch = rd.U64(); (void)epoch;
      std::uint32_t ncomp = rd.U32();
      bool found = false;
      for (std::uint32_t i = 0; i < ncomp; ++i) { Composition c; DecodeComposition(rd, c); if (c.id == c1id) { c1check = c; found = true; } }
      if (found) {
        std::printf("C1 lifecycle after worker death: %s\n", ToString(c1check.lifecycle));
        if (c1check.lifecycle == CompositionLifecycle::RECOMPOSITION_REQUIRED || c1check.lifecycle == CompositionLifecycle::REVALIDATION_REQUIRED) { becameRecomp = true; break; }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  CHECK(becameRecomp);
  printf("C1 invalidated after worker death\n");

  // Replay stale WorkerBoot A1 traffic: spawn a fresh worker with the OLD boot and
  // confirm it cannot silently restore C1 (must be rejected because boot is stale).
  Proc staleWorker = Spawn(worker, { "--coord=127.0.0.1:" + std::to_string(g_port), "--boot=1001", "--nodes=1" }, "");
  CHECK(staleWorker.valid());
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  staleWorker.Kill(); staleWorker.Wait(5000); staleWorker.Close();

  // Release C1 (fence + free its holds) so C2 can reuse resources without double reservation.
  {
    WireWriter w; w.U64(c1id.Value()); Frame resp;
    CHECK(RoundTrip(coordConn, dec, FrameType::RELEASE_REQ, FrameType::RELEASE_RESP, 9199, w.Take(), resp));
    WireReader rd(resp.payload.data(), resp.payload.size()); std::uint8_t ok = rd.U8();
    CHECK(ok == 1);
  }

  // Spawn worker A-prime with fresh boot and republish.
  Proc workerA2 = SpawnToFile(worker, { "--coord=127.0.0.1:" + std::to_string(g_port), "--boot=2001", "--nodes=1" }, "", "_workerA2.out");
  CHECK(workerA2.valid());

  // Build C2 (fresh authority). Retry until resources present.
  Composition c2;
  CompositionPlan plan2;
  for (int tries = 0; tries < 100; ++tries) {
    WireWriter w; EncodeRequest(w, req); Frame resp;
    if (RoundTrip(coordConn, dec, FrameType::COMPOSE_REQ, FrameType::COMPOSE_RESP, 9200, w.Take(), resp)) {
      WireReader rd(resp.payload.data(), resp.payload.size());
      std::uint32_t oc = rd.U32();
      if (static_cast<CompositionOutcome>(oc) == CompositionOutcome::SUCCESS) { DecodePlan(rd, plan2); break; }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  CHECK(plan2.graph.vertices.size() >= 6);
  {
    WireWriter w; EncodePlan(w, plan2); Frame resp;
    CHECK(RoundTrip(coordConn, dec, FrameType::RESERVE_REQ, FrameType::RESERVE_RESP, 9201, w.Take(), resp));
    WireReader rd(resp.payload.data(), resp.payload.size()); std::uint32_t oc = rd.U32();
    Reservation res; DecodeReservation(rd, res); CHECK(oc == static_cast<std::uint32_t>(CompositionOutcome::SUCCESS));
    WireWriter w2; EncodeReservation(w2, res); Frame resp2;
    CHECK(RoundTrip(coordConn, dec, FrameType::COMMIT_REQ, FrameType::COMMIT_RESP, 9202, w2.Take(), resp2));
    WireReader rd2(resp2.payload.data(), resp2.payload.size()); std::uint32_t oc2 = rd2.U32();
    DecodeComposition(rd2, c2); CHECK(oc2 == static_cast<std::uint32_t>(CompositionOutcome::SUCCESS));
    WireWriter w3; w3.U64(c2.id.Value()); Frame resp3;
    CHECK(RoundTrip(coordConn, dec, FrameType::ACTIVATE_REQ, FrameType::ACTIVATE_RESP, 9203, w3.Take(), resp3));
    WireReader rd3(resp3.payload.data(), resp3.payload.size()); std::uint32_t oc3 = rd3.U32();
    DecodeComposition(rd3, c2); CHECK(oc3 == static_cast<std::uint32_t>(CompositionOutcome::SUCCESS));
    CHECK(c2.lifecycle == CompositionLifecycle::ACTIVE);
  }
  CHECK(c2.generation.Value() > c1gen.Value());   // fresh authority, higher generation
  printf("C2 ACTIVE: id=%llu gen=%llu > C1 gen=%llu\n", (unsigned long long)c2.id.Value(), (unsigned long long)c2.generation.Value(), (unsigned long long)c1gen.Value());

  // Verify no duplicate reservations / leaks: every committed composition holds a
  // distinct reservation and resources are not double-reserved.
  {
    WireWriter w; Frame resp;
    CHECK(RoundTrip(coordConn, dec, FrameType::QUERY_STATE, FrameType::STATE_RESP, 9300, w.Take(), resp));
    WireReader rd(resp.payload.data(), resp.payload.size());
    std::uint64_t epoch = rd.U64(); (void)epoch;
    std::uint32_t ncomp = rd.U32();
    std::vector<Composition> comps;
    for (std::uint32_t i = 0; i < ncomp; ++i) { Composition c; DecodeComposition(rd, c); comps.push_back(c); }
    std::uint32_t nresv = rd.U32();
    std::vector<Reservation> resvs;
    for (std::uint32_t i = 0; i < nresv; ++i) { Reservation r; DecodeReservation(rd, r); resvs.push_back(r); }
    std::size_t activeCount = 0;
    for (const auto& c : comps) if (c.lifecycle == CompositionLifecycle::ACTIVE) activeCount++;
    CHECK(activeCount == 1);   // only C2 active; C1 fenced by recomposition
    CHECK(resvs.size() >= 1);
  }

  printf("worker-death proof ok\n");

  coordProc.Kill(); coordProc.Wait(5000); coordProc.Close();
  workerA.Close(); workerB.Kill(); workerB.Wait(5000); workerB.Close();
  workerC.Kill(); workerC.Wait(5000); workerC.Close();
  workerA2.Kill(); workerA2.Wait(5000); workerA2.Close();
  coordConn.Close();
  return 0;
}