// Disaggregation Fabric — runnable examples of real implemented behavior.
#include "dfabric/engine.hpp"
#include "synthetic_backend.hpp"
#include <cstdio>
#include <memory>

using namespace dfabric;

static std::shared_ptr<SyntheticBackend> Fresh(FabricEngine& e) { auto s = std::make_shared<SyntheticBackend>(NodeGeneration(1), CompositionRequest{}); e.AddBackend(s); e.Discover(); return s; }
static CompositionRequest MakeReq() {
  CompositionRequest q; q.compute_units = 1; q.accelerator_count = 1; q.accelerator_memory_bytes = 1.0;
  q.host_memory_bytes = 1024; q.expanded_memory_bytes = 1024; q.storage_bytes = 1024; q.network_endpoint_count = 1;
  q.min_bandwidth_mbps = 1000; q.max_latency_us = 100;
  q.mandatory_classes.insert(ResourceClass::COMPUTE); q.mandatory_classes.insert(ResourceClass::ACCELERATOR);
  q.mandatory_classes.insert(ResourceClass::HOST_MEMORY); q.mandatory_classes.insert(ResourceClass::EXPANDED_MEMORY);
  q.mandatory_classes.insert(ResourceClass::STORAGE); q.mandatory_classes.insert(ResourceClass::NETWORK_ENDPOINT);
  return q;
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  // basic_composition
  {
    EngineConfig cfg; cfg.coordinator = CoordinatorId(1); cfg.epoch = CoordinatorEpoch(1);
    FabricEngine e(cfg); Fresh(e);
    auto cr = e.Compose(MakeReq()); std::printf("basic_composition: %s vertices=%zu\n", cr.ok()?"SUCCESS":"REJECTED", cr.plan.graph.vertices.size());
    auto rr = e.Reserve(cr.plan); auto cc = e.Commit(rr.reservation); auto ar = e.Activate(cc.composition.id);
    std::printf("  reserve=%s commit=%s activate=%s lifecycle=%s\n", rr.ok()?"ok":"rej", cc.ok()?"ok":"rej", ar.ok()?"ok":"rej", ToString(ar.composition.lifecycle));
  }

  // hard_constraint_rejection
  {
    EngineConfig cfg; cfg.coordinator = CoordinatorId(1); cfg.epoch = CoordinatorEpoch(1);
    FabricEngine e(cfg); auto syn = Fresh(e);
    syn->SetResourceHealth(ResourceId(102), ResourceHealth::OFFLINE); syn->SetResourceHealth(ResourceId(106), ResourceHealth::OFFLINE);
    e.OnResourceFailure(ResourceId(102)); e.OnResourceFailure(ResourceId(106));
    auto cr = e.Compose(MakeReq());
    std::printf("hard_constraint_rejection: %s reasons=", cr.ok()?"SUCCESS":"REJECTED");
    for (auto r : cr.reasons) std::printf("%s ", ToString(r));
    std::printf("\n");
  }

  // reservation_rollback
  {
    EngineConfig cfg; cfg.coordinator = CoordinatorId(1); cfg.epoch = CoordinatorEpoch(1);
    FabricEngine e(cfg); auto syn = Fresh(e);
    auto cr = e.Compose(MakeReq());
    syn->SetResourceUnavailable(ResourceId(102)); syn->SetResourceUnavailable(ResourceId(106));
    e.OnResourceFailure(ResourceId(102)); e.OnResourceFailure(ResourceId(106));
    auto rr = e.Reserve(cr.plan);
    std::printf("reservation_rollback: reserve=%s (no partial authoritative set)\n", rr.ok()?"SUCCESS":"REJECTED");
  }

  // resource_generation_change
  {
    EngineConfig cfg; cfg.coordinator = CoordinatorId(1); cfg.epoch = CoordinatorEpoch(1);
    FabricEngine e(cfg); auto syn = Fresh(e);
    auto cr = e.Compose(MakeReq()); auto rr = e.Reserve(cr.plan); auto cc = e.Commit(rr.reservation); auto ar = e.Activate(cc.composition.id);
    syn->SetResourceGeneration(ResourceId(102), ResourceGeneration(9)); syn->SetResourceGeneration(ResourceId(106), ResourceGeneration(9)); e.Discover();
    auto after = e.GetComposition(ar.composition.id);
    std::printf("resource_generation_change: %s -> %s\n", ToString(CompositionLifecycle::ACTIVE), ToString(after->lifecycle));
  }

  // recomposition (fresh authority)
  {
    EngineConfig cfg; cfg.coordinator = CoordinatorId(1); cfg.epoch = CoordinatorEpoch(1);
    FabricEngine e(cfg); auto syn = Fresh(e);
    auto cr = e.Compose(MakeReq()); auto rr = e.Reserve(cr.plan); auto cc = e.Commit(rr.reservation); auto ar = e.Activate(cc.composition.id);
    auto accel = ar.composition.graph.vertices; ResourceId am{};
    for (auto v : accel) { auto res = e.GetResource(v); if (res && res->resource_class == ResourceClass::ACCELERATOR) am = v; }
    syn->SetResourceHealth(am, ResourceHealth::OFFLINE); e.OnResourceFailure(am);
    bool ok = e.Recompute(ar.composition.id);
    auto snap = e.Snapshot(); std::size_t active = 0; Composition latest;
    for (auto& c : snap.compositions) { if (c.lifecycle == CompositionLifecycle::ACTIVE) { active++; latest = c; } }
    std::printf("recomposition: ok=%d active=%zu latest_gen=%llu\n", ok?1:0, active, (unsigned long long)latest.generation.Value());
  }

  // synthetic_multinode
  {
    EngineConfig cfg; cfg.coordinator = CoordinatorId(1); cfg.epoch = CoordinatorEpoch(1);
    FabricEngine e(cfg); auto syn = Fresh(e);
    auto cr = e.Compose(MakeReq());
    std::printf("synthetic_multinode: %s vertices=%zu (SYNTHETIC topology)\n", cr.ok()?"SUCCESS":"REJECTED", cr.plan.graph.vertices.size());
    std::printf("  nodes: "); for (auto n : cr.plan.nodes) std::printf("%s ", n.ToString().c_str()); std::printf("\n");
  }

  std::printf("\nexamples complete\n");
  return 0;
}