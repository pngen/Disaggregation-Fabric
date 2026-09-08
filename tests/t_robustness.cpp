// Disaggregation Fabric — robustness: seeded property + concurrency + adversarial.
#include "test_util.hpp"
#include "dfabric/engine.hpp"
#include "synthetic_backend.hpp"
#include <memory>
#include <thread>
#include <atomic>
#include <vector>
#include <algorithm>

using namespace dfabric;

static CompositionRequest MakeReq() {
  CompositionRequest req; req.compute_units = 1; req.accelerator_count = 1; req.accelerator_memory_bytes = 1.0;
  req.host_memory_bytes = 1024; req.expanded_memory_bytes = 1024; req.storage_bytes = 1024; req.network_endpoint_count = 1;
  req.min_bandwidth_mbps = 1000; req.max_latency_us = 100;
  req.mandatory_classes.insert(ResourceClass::COMPUTE); req.mandatory_classes.insert(ResourceClass::ACCELERATOR);
  req.mandatory_classes.insert(ResourceClass::HOST_MEMORY); req.mandatory_classes.insert(ResourceClass::EXPANDED_MEMORY);
  req.mandatory_classes.insert(ResourceClass::STORAGE); req.mandatory_classes.insert(ResourceClass::NETWORK_ENDPOINT);
  return req;
}

static std::shared_ptr<SyntheticBackend> Fresh(FabricEngine& e) { auto s = std::make_shared<SyntheticBackend>(NodeGeneration(1), CompositionRequest{}); e.AddBackend(s); e.Discover(); return s; }

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0); std::setvbuf(stderr, nullptr, _IONBF, 0);

  // --- seeded property: success implies a valid graph, failure leaves no leak ---
  {
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
      EngineConfig cfg; cfg.coordinator = CoordinatorId(1); cfg.epoch = CoordinatorEpoch(1);
      FabricEngine e(cfg); auto syn = Fresh(e);
      // Randomize health/reachability deterministically.
      auto res = syn->DiscoverResources();
      for (auto& r : res) {
        std::uint64_t h = (seed * 31 + r.id.Value()) % 7;
        if (h == 0) { syn->SetResourceHealth(r.id, ResourceHealth::OFFLINE); e.OnResourceFailure(r.id); }
        else if (h == 5) { syn->SetResourceReachability(r.id, Reachability::UNREACHABLE); e.OnResourceFailure(r.id); }
      }
      auto cr = e.Compose(MakeReq());
      if (cr.ok()) {
        // Every mandatory edge must be present and the graph non-empty.
        CHECK(cr.plan.graph.vertices.size() >= 5);
        CHECK(cr.plan.authority.coordinator_epoch == CoordinatorEpoch(1));
        // Reserve+commit+activate must work.
        auto rr = e.Reserve(cr.plan); CHECK(rr.ok());
        auto cc = e.Commit(rr.reservation); CHECK(cc.ok());
        auto ar = e.Activate(cc.composition.id); CHECK(ar.ok());
      } else {
        // Failure path: no reservation should have been created/held.
        CHECK(e.Snapshot().reservations.empty());
      }
    }
    printf("property: seed iterations ok\n");
  }

  // --- concurrency: two compositions cannot both reserve the same exclusive resource ---
  {
    EngineConfig cfg; cfg.coordinator = CoordinatorId(1); cfg.epoch = CoordinatorEpoch(1);
    FabricEngine e(cfg); Fresh(e);
    auto cr = e.Compose(MakeReq());
    CHECK(cr.ok());
    CompositionPlan p = cr.plan;
    std::atomic<int> success{0};
    auto worker = [&]() {
      auto rr = e.Reserve(p);
      if (rr.ok()) { success++; e.Commit(rr.reservation); }
    };
    std::thread a(worker), b(worker);
    a.join(); b.join();
    CHECK(success == 1);   // exactly one holds the exclusive set
    printf("concurrency: exclusive reservation conflict ok\n");
  }

  // --- adversarial: duplicate registration, double release, zero ID reject ---
  {
    EngineConfig cfg; cfg.coordinator = CoordinatorId(1); cfg.epoch = CoordinatorEpoch(1);
    FabricEngine e(cfg); Fresh(e);
    // Duplicate registration of the same resource (with a bumped generation).
    auto r = e.GetResource(ResourceId(102)); CHECK(r.has_value());
    Resource dup = *r; dup.generation = ResourceGeneration(9);
    CHECK(e.RegisterResource(dup));
    // Duplicate node registration.
    auto n = e.GetNode(NodeId(1)); CHECK(n.has_value());
    CHECK(e.RegisterNode(*n));
    // Zero ID must be rejected on registration.
    Resource z; z.id = ResourceId(0);
    CHECK(!e.RegisterResource(z));
    // Double release of a released composition.
    auto cr = e.Compose(MakeReq()); CHECK(cr.ok());
    auto rr = e.Reserve(cr.plan); CHECK(rr.ok());
    auto cc = e.Commit(rr.reservation); CHECK(cc.ok());
    auto ar = e.Activate(cc.composition.id); CHECK(ar.ok());
    CHECK(e.BeginDrain(cc.composition.id));
    CHECK(e.EndDrain(cc.composition.id));
    CHECK(!e.Release(cc.composition.id));   // double release rejected
    printf("adversarial: duplicate/double-release ok\n");
  }

  // --- adversarial: capacity shrink during hold causes commit rejection ---
  {
    EngineConfig cfg; cfg.coordinator = CoordinatorId(1); cfg.epoch = CoordinatorEpoch(1);
    FabricEngine e(cfg); auto syn = Fresh(e);
    auto cr = e.Compose(MakeReq()); CHECK(cr.ok());
    auto rr = e.Reserve(cr.plan); CHECK(rr.ok());
    // Withdraw a held member so commit revalidation fails -> rollback.
    auto accel = e.GetComposition(cr.plan.id);
    (void)accel;
    // Find an accelerator in the plan and withdraw it.
    ResourceId member{};
    for (auto& v : cr.plan.graph.vertices) { auto res = e.GetResource(v); if (res && res->resource_class == ResourceClass::ACCELERATOR) { member = v; break; } }
    CHECK(member.IsValid());
    syn->SetResourceUnavailable(member);
    e.OnResourceFailure(member);
    auto cc = e.Commit(rr.reservation);
    CHECK(!cc.ok());   // commit rejected; rolled back
    CHECK(cc.composition.lifecycle == CompositionLifecycle::REVALIDATION_REQUIRED);
    printf("adversarial: commit rollback on member failure ok\n");
  }

  printf("robustness test ok\n");
  return 0;
}
