// Disaggregation Fabric — regression tests: exclusive holds are released once, stale plans
// are refused at reserve and activation, the epoch never moves backwards, and the state file
// is replaced atomically even when several threads save.
#include "dfabric/engine.hpp"
#include "synthetic_backend.hpp"
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace dfabric;

static int g_failures = 0;
#define EXPECT(cond) do { if (!(cond)) { ++g_failures; std::printf("FAILED: %s (%s:%d)\n", #cond, __FILE__, __LINE__); } } while (0)

static CompositionRequest MakeReq() {
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
  return req;
}

static EngineConfig Cfg() { EngineConfig cfg; cfg.coordinator = CoordinatorId(1); cfg.epoch = CoordinatorEpoch(1); return cfg; }

static std::shared_ptr<SyntheticBackend> Fresh(FabricEngine& e) {
  auto s = std::make_shared<SyntheticBackend>(NodeGeneration(1), CompositionRequest{});
  e.AddBackend(s);
  e.Discover();
  return s;
}

// Compose, reserve, commit and activate; an invalid id when any step fails.
static CompositionId Establish(FabricEngine& e, const CompositionRequest& req) {
  auto cr = e.Compose(req); if (!cr.ok()) return CompositionId();
  auto rr = e.Reserve(cr.plan); if (!rr.ok()) return CompositionId();
  auto cc = e.Commit(rr.reservation); if (!cc.ok()) return CompositionId();
  auto ar = e.Activate(cc.composition.id); if (!ar.ok()) return CompositionId();
  return ar.composition.id;
}

// No resource is held by two live compositions at once.
static bool HoldsDisjoint(FabricEngine& e) {
  std::map<std::uint64_t, int> users;
  for (const auto& c : e.Snapshot().compositions) {
    if (c.lifecycle != CompositionLifecycle::ACTIVE && c.lifecycle != CompositionLifecycle::COMMITTED &&
        c.lifecycle != CompositionLifecycle::RESERVED) continue;
    for (const auto& v : c.graph.vertices) if (++users[v.Value()] > 1) return false;
  }
  return true;
}

static PathEvidence Withdrawn(const PathEvidence& p) {
  PathEvidence gone = p;
  gone.available = false; gone.verified = false;
  gone.generation = PathGeneration(p.generation.Value() + 1);
  return gone;
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  // Recomposing a released composition must not take over another composition's holds.
  {
    FabricEngine e(Cfg()); Fresh(e);
    CompositionId a = Establish(e, MakeReq());
    EXPECT(a.IsValid());
    EXPECT(e.Release(a));
    CompositionId b = Establish(e, MakeReq());   // the same exclusive resources
    EXPECT(b.IsValid());
    EXPECT(!e.Recompute(a));                      // nothing is free while B holds them
    EXPECT(HoldsDisjoint(e));
    auto cr = e.Compose(MakeReq());
    if (cr.ok()) EXPECT(!e.Reserve(cr.plan).ok());
  }

  // Releasing a composition whose commit was rolled back must not free another's holds.
  {
    FabricEngine e(Cfg()); auto syn = Fresh(e);
    auto crA = e.Compose(MakeReq());
    EXPECT(crA.ok());
    auto rrA = e.Reserve(crA.plan);
    EXPECT(rrA.ok());
    ResourceId member{};
    for (auto& v : crA.plan.graph.vertices) { auto r = e.GetResource(v); if (r && r->resource_class == ResourceClass::ACCELERATOR) { member = v; break; } }
    EXPECT(member.IsValid());
    syn->SetResourceUnavailable(member); e.OnResourceFailure(member);
    EXPECT(!e.Commit(rrA.reservation).ok());     // rolled back: A's holds are returned
    syn->SetResourceBack(member); e.Discover();
    CompositionId b = Establish(e, MakeReq());
    EXPECT(b.IsValid());
    e.Release(crA.plan.id);
    auto cr = e.Compose(MakeReq());
    if (cr.ok()) EXPECT(!e.Reserve(cr.plan).ok());
    EXPECT(HoldsDisjoint(e));
  }

  // A plan whose cross-node path was withdrawn is not reserved; a committed composition whose
  // path was withdrawn is not activated.
  {
    FabricEngine e(Cfg()); Fresh(e);
    auto cr = e.Compose(MakeReq());
    EXPECT(cr.ok() && !cr.plan.paths.empty());
    if (cr.ok() && !cr.plan.paths.empty()) {
      e.OnPathUpdate(Withdrawn(cr.plan.paths.front()));
      EXPECT(!e.Reserve(cr.plan).ok());
    }
  }
  {
    FabricEngine e(Cfg()); Fresh(e);
    auto cr = e.Compose(MakeReq());
    auto rr = e.Reserve(cr.plan);
    auto cc = e.Commit(rr.reservation);
    EXPECT(cc.ok() && !cr.plan.paths.empty());
    if (cc.ok() && !cr.plan.paths.empty()) {
      e.OnPathUpdate(Withdrawn(cr.plan.paths.front()));
      EXPECT(!e.Activate(cc.composition.id).ok());
    }
  }

  // A policy generation that moved while the composition was reserved blocks activation.
  {
    FabricEngine e(Cfg()); Fresh(e);
    CompositionRequest req = MakeReq();
    req.policy = PolicyId(9); req.policy_generation = PolicyGeneration(1);
    auto cr = e.Compose(req);
    auto rr = e.Reserve(cr.plan);
    EXPECT(rr.ok());
    e.OnPolicyUpdate(PolicyId(9), PolicyGeneration(2));
    auto cc = e.Commit(rr.reservation);
    if (cc.ok()) EXPECT(!e.Activate(cc.composition.id).ok());
  }

  // A composition id is reserved once: a released composition stays released.
  {
    FabricEngine e(Cfg()); Fresh(e);
    auto cr = e.Compose(MakeReq());
    auto rr = e.Reserve(cr.plan);
    auto cc = e.Commit(rr.reservation);
    EXPECT(cc.ok());
    EXPECT(e.Release(cr.plan.id));
    EXPECT(!e.Reserve(cr.plan).ok());
    auto c = e.GetComposition(cr.plan.id);
    EXPECT(c.has_value() && c->lifecycle == CompositionLifecycle::RELEASED);
  }

  // A plan made under an earlier epoch is not reserved.
  {
    FabricEngine e(Cfg()); Fresh(e);
    auto cr = e.Compose(MakeReq());
    EXPECT(cr.ok());
    e.AdvanceEpoch(CoordinatorEpoch(2));
    EXPECT(!e.Reserve(cr.plan).ok());
  }

  // The epoch never moves backwards.
  {
    FabricEngine e(Cfg());
    e.AdvanceEpoch(CoordinatorEpoch(5));
    e.AdvanceEpoch(CoordinatorEpoch(3));
    EXPECT(e.epoch() == CoordinatorEpoch(5));
  }

  // Saving replaces the state file atomically, also with several threads saving: whenever a
  // reader can open it, the file is complete (never truncated or interleaved). Opening it can
  // fail transiently while a rename replaces it; that is not counted.
  {
    const std::string path = (std::filesystem::temp_directory_path() / "dfabric_regression_state.bin").string();
    std::remove(path.c_str());
    FabricEngine e(Cfg()); Fresh(e);
    EXPECT(Establish(e, MakeReq()).IsValid());
    EXPECT(e.Save(path));
    std::atomic<bool> stop{ false };
    std::atomic<int> badLoads{ 0 };
    std::thread reader([&] {
      while (!stop) {
        FabricEngine r(Cfg());
        LoadResult lr = r.Load(path);
        if (!lr.ok && lr.error != "cannot open path for read") ++badLoads;
      }
    });
    std::vector<std::thread> writers;
    for (int t = 0; t < 2; ++t) writers.emplace_back([&] { for (int i = 0; i < 100; ++i) e.Save(path); });
    for (auto& w : writers) w.join();
    stop = true;
    reader.join();
    EXPECT(badLoads == 0);
    FabricEngine f(Cfg());
    EXPECT(f.Load(path).ok);
    std::remove(path.c_str());
    std::remove((path + ".tmp").c_str());
  }

  std::printf("regression: %d failure(s)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
