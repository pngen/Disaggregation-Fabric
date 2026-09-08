// Disaggregation Fabric — reference coordinator process.
// Runs a FabricEngine over a synthetic topology, serves workers over real framed
// TCP, and exposes a control channel that performs compose / reserve / commit /
// activate / state-query / save. Worker disconnect (hard OS kill included) is
// detected from the socket and invalidates that worker's dynamic evidence.
#include "dfabric/engine.hpp"
#include "dfabric/protocol.hpp"
#include "dfabric/socket.hpp"
#include "dfabric/codec.hpp"
#include "synthetic_backend.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <atomic>
#include <vector>
#include <set>
#include <string>
#include <fstream>
#include <chrono>

using namespace dfabric;
using namespace dfabric::proto;
using namespace dfabric::net;

namespace {
std::string g_state_path;
std::string g_markdir;

void WriteMarker(const std::string& name) {
  if (g_markdir.empty()) return;
  std::string p = g_markdir + "/" + name;
  std::ofstream f(p, std::ios::trunc); f << "1\n"; f.close();
}
bool ReadBool(const std::vector<std::uint8_t>& p) { WireReader r(p.data(), p.size()); return r.U8() != 0; }
}

int main(int argc, char** argv) {
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  int port = 0;
  std::uint64_t epochVal = 1;
  std::string statePath = "coordinator.state";
  std::string markdir;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a.rfind("--port=", 0) == 0) port = std::atoi(a.substr(7).c_str());
    if (a.rfind("--epoch=", 0) == 0) epochVal = std::strtoull(a.substr(8).c_str(), nullptr, 10);
    if (a.rfind("--state=", 0) == 0) statePath = a.substr(8);
    if (a.rfind("--markdir=", 0) == 0) markdir = a.substr(10);
  }
  if (port <= 0) { std::fprintf(stderr, "coordinator: --port required\n"); return 1; }
  g_state_path = statePath;
  g_markdir = markdir;

  EngineConfig cfg; cfg.coordinator = CoordinatorId(1); cfg.epoch = CoordinatorEpoch(1);
  FabricEngine engine(cfg);
  auto syn = std::make_shared<SyntheticBackend>(NodeGeneration(1), CompositionRequest{});
  engine.AddBackend(syn);

  // Seed paths from the synthetic topology (resources come from workers).
  {
    auto nodes = syn->DiscoverNodes();
    for (const auto& a : nodes) {
      for (const auto& b : nodes) {
        if (a.id == b.id) continue;
        PathRequirement pr; pr.from = a.id; pr.to = b.id; pr.required = true; pr.min_bandwidth_mbps = 1000; pr.max_latency_us = 100;
        auto pe = syn->ResolvePath(pr);
        if (pe) engine.OnPathUpdate(*pe);
      }
    }
  }

  // Load prior durable state (conservative; nothing regains authority).
  if (std::ifstream(statePath).good()) {
    LoadResult lr = engine.Load(statePath);
    if (!lr.ok) std::fprintf(stderr, "coordinator: load warning: %s\n", lr.error.c_str());
    engine.AdvanceEpoch(CoordinatorEpoch(epochVal));
  } else {
    engine.AdvanceEpoch(CoordinatorEpoch(epochVal));
  }

  ServerSocket server;
  if (!server.Listen(port)) { std::fprintf(stderr, "coordinator: listen failed on %d\n", port); return 1; }
  std::fprintf(stderr, "coordinator: listening on %d epoch=%llu\n", port, (unsigned long long)epochVal);

  std::atomic<bool> running{ true };
  std::vector<std::thread> threads;

  while (running) {
    auto conn = server.Accept();
    if (!conn.Valid()) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
    threads.emplace_back([conn = std::move(conn), &engine, &syn, &running]() mutable {
      FrameDecoder dec;
      std::uint64_t boot = 0;
      bool isWorker = false;
      bool done = false;
      while (!done && running) {
        std::uint8_t buf[8192];
        long got = conn.RecvSome(buf, 8192);
        if (got == -2) { std::this_thread::sleep_for(std::chrono::milliseconds(3)); continue; }
        if (got == 0) { done = true; break; }   // clean EOF / peer closed
        if (got < 0) { done = true; break; }    // network error (e.g. worker killed)
        std::vector<Frame> frames; std::string err;
        if (!dec.Feed(buf, static_cast<std::size_t>(got), frames, err)) { done = true; break; }
        for (auto& f : frames) {
          const std::uint64_t corr = f.correlation;
          if (f.type == FrameType::HELLO) {
            WireReader rd(f.payload.data(), f.payload.size());
            boot = rd.U64(); isWorker = (boot != 0);
            auto ack = Encode(FrameType::ACK, corr, nullptr, 0);
            for (size_t off = 0; off < ack.size(); ) { long sr = conn.SendSome(ack.data() + off, (long)(ack.size() - off)); if (sr < 0) { done = true; break; } if (sr > 0) off += sr; }
          } else if (f.type == FrameType::REGISTER) {
            WireReader rd(f.payload.data(), f.payload.size());
            std::uint32_t n = rd.U32();
            std::vector<NodeId> ids; for (std::uint32_t i = 0; i < n; ++i) ids.push_back(NodeId(rd.U64()));
            if (!isWorker) { /* control clients may also register; ignore for worker semantics */ }
            // Serve descriptors to the worker so it can republish them.
            auto nodes = syn->DiscoverNodes();
            auto res = syn->DiscoverResources();
            for (auto nid : ids) {
              for (auto& nd : nodes) if (nd.id == nid) {
                WireWriter w; EncodeNode(w, nd); auto b = w.Take();
                auto fr = Encode(FrameType::PUBLISH_NODE, corr, b.data(), b.size());
                for (size_t off = 0; off < fr.size(); ) { long sr = conn.SendSome(fr.data() + off, (long)(fr.size() - off)); if (sr < 0) { done = true; break; } if (sr > 0) off += sr; }
              }
              for (auto& r : res) if (r.node_id == nid) {
                WireWriter w; EncodeResource(w, r); auto b = w.Take();
                auto fr = Encode(FrameType::PUBLISH_RESOURCE, corr, b.data(), b.size());
                for (size_t off = 0; off < fr.size(); ) { long sr = conn.SendSome(fr.data() + off, (long)(fr.size() - off)); if (sr < 0) { done = true; break; } if (sr > 0) off += sr; }
              }
            }
            auto ack = Encode(FrameType::ACK, corr, nullptr, 0);
            for (size_t off = 0; off < ack.size(); ) { long sr = conn.SendSome(ack.data() + off, (long)(ack.size() - off)); if (sr < 0) { done = true; break; } if (sr > 0) off += sr; }
          } else if (f.type == FrameType::PUBLISH_NODE) {
            Node n; WireReader rd(f.payload.data(), f.payload.size());
            if (DecodeNode(rd, n) && isWorker) { n.worker_boot = WorkerBootId(boot); n.worker_owner = std::to_string(boot); engine.RegisterNode(n); }
          } else if (f.type == FrameType::PUBLISH_RESOURCE) {
            Resource res; WireReader rd(f.payload.data(), f.payload.size());
            if (DecodeResource(rd, res) && isWorker) { res.evidence.owner = std::to_string(boot); res.authority_owner = std::to_string(boot); res.evidence.durable = false; engine.RegisterResource(res); }
          } else if (f.type == FrameType::COMPOSE_REQ) {
            CompositionRequest req; WireReader rd(f.payload.data(), f.payload.size());
            DecodeRequest(rd, req);
            auto cr = engine.Compose(req);
            WireWriter w; w.U32(static_cast<std::uint32_t>(cr.outcome)); EncodePlan(w, cr.plan);
            w.U32(static_cast<std::uint32_t>(cr.reasons.size()));
            for (auto reason : cr.reasons) w.U32(static_cast<std::uint32_t>(reason));
            auto b = w.Take();
            auto fr = Encode(FrameType::COMPOSE_RESP, corr, b.data(), b.size());
            for (size_t off = 0; off < fr.size(); ) { long sr = conn.SendSome(fr.data() + off, (long)(fr.size() - off)); if (sr < 0) { done = true; break; } if (sr > 0) off += sr; }
          } else if (f.type == FrameType::RESERVE_REQ) {
            CompositionPlan plan; WireReader rd(f.payload.data(), f.payload.size());
            DecodePlan(rd, plan);
            auto rr = engine.Reserve(plan);
            WireWriter w; w.U32(static_cast<std::uint32_t>(rr.outcome)); EncodeReservation(w, rr.reservation);
            for (auto reason : rr.reasons) w.U32(static_cast<std::uint32_t>(reason));
            auto b = w.Take();
            auto fr = Encode(FrameType::RESERVE_RESP, corr, b.data(), b.size());
            for (size_t off = 0; off < fr.size(); ) { long sr = conn.SendSome(fr.data() + off, (long)(fr.size() - off)); if (sr < 0) { done = true; break; } if (sr > 0) off += sr; }
            if (rr.ok()) { engine.Save(g_state_path); WriteMarker("reserved"); }
          } else if (f.type == FrameType::COMMIT_REQ) {
            Reservation res; WireReader rd(f.payload.data(), f.payload.size());
            DecodeReservation(rd, res);
            auto cc = engine.Commit(res);
            WireWriter w; w.U32(static_cast<std::uint32_t>(cc.outcome)); EncodeComposition(w, cc.composition);
            auto b = w.Take();
            auto fr = Encode(FrameType::COMMIT_RESP, corr, b.data(), b.size());
            for (size_t off = 0; off < fr.size(); ) { long sr = conn.SendSome(fr.data() + off, (long)(fr.size() - off)); if (sr < 0) { done = true; break; } if (sr > 0) off += sr; }
            if (cc.ok()) { engine.Save(g_state_path); WriteMarker("committed"); }
          } else if (f.type == FrameType::ACTIVATE_REQ) {
            CompositionId cid; WireReader rd(f.payload.data(), f.payload.size()); cid = CompositionId(rd.U64());
            auto ar = engine.Activate(cid);
            WireWriter w; w.U32(static_cast<std::uint32_t>(ar.outcome)); EncodeComposition(w, ar.composition);
            auto b = w.Take();
            auto fr = Encode(FrameType::ACTIVATE_RESP, corr, b.data(), b.size());
            for (size_t off = 0; off < fr.size(); ) { long sr = conn.SendSome(fr.data() + off, (long)(fr.size() - off)); if (sr < 0) { done = true; break; } if (sr > 0) off += sr; }
          } else if (f.type == FrameType::QUERY_STATE) {
            auto snap = engine.Snapshot();
            WireWriter w; w.U64(snap.epoch.Value());
            w.U32(static_cast<std::uint32_t>(snap.compositions.size()));
            for (const auto& c : snap.compositions) EncodeComposition(w, c);
            w.U32(static_cast<std::uint32_t>(snap.reservations.size()));
            for (const auto& resv : snap.reservations) EncodeReservation(w, resv);
            w.U32(static_cast<std::uint32_t>(snap.resources.size()));
            for (const auto& res : snap.resources) EncodeResource(w, res);
            auto b = w.Take();
            auto fr = Encode(FrameType::STATE_RESP, corr, b.data(), b.size());
            for (size_t off = 0; off < fr.size(); ) { long sr = conn.SendSome(fr.data() + off, (long)(fr.size() - off)); if (sr < 0) { done = true; break; } if (sr > 0) off += sr; }
          } else if (f.type == FrameType::RELEASE_REQ) {
            CompositionId cid; WireReader rd(f.payload.data(), f.payload.size()); cid = CompositionId(rd.U64());
            bool ok = engine.Release(cid);
            WireWriter w; w.U8(ok ? 1 : 0); auto b = w.Take();
            auto fr = Encode(FrameType::RELEASE_RESP, corr, b.data(), b.size());
            for (size_t off = 0; off < fr.size(); ) { long sr = conn.SendSome(fr.data() + off, (long)(fr.size() - off)); if (sr < 0) { done = true; break; } if (sr > 0) off += sr; }
          } else if (f.type == FrameType::SAVE_REQ) {
            engine.Save(g_state_path);
            auto ack = Encode(FrameType::SAVE_RESP, corr, nullptr, 0);
            for (size_t off = 0; off < ack.size(); ) { long sr = conn.SendSome(ack.data() + off, (long)(ack.size() - off)); if (sr < 0) { done = true; break; } if (sr > 0) off += sr; }
          } else if (f.type == FrameType::HEARTBEAT) {
            auto ack = Encode(FrameType::HEARTBEAT, corr, nullptr, 0);
            for (size_t off = 0; off < ack.size(); ) { long sr = conn.SendSome(ack.data() + off, (long)(ack.size() - off)); if (sr < 0) { done = true; break; } if (sr > 0) off += sr; }
          } else if (f.type == FrameType::BYE) { done = true; }
        }
      }
      // Worker disconnect: invalidate its dynamic evidence.
      if (isWorker && boot != 0) engine.MarkEvidenceRevalidationRequired(WorkerBootId(boot));
      conn.Close();
    });
  }
  for (auto& t : threads) if (t.joinable()) t.join();
  return 0;
}