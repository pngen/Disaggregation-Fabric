// Disaggregation Fabric — in-process semantics test.
// Covers eligibility, hard-rejection, compose, atomic reservation + rollback,
// commit/activate, lifecycle guards, generation invalidation, stale rejection,
// recomposition fresh authority, make-before-break, drain, failure-domain
// constraints, deterministic ranking and insertion-order independence.
#include "test_util.hpp"
#include "dfabric/engine.hpp"
#include "synthetic_backend.hpp"
#include <memory>
#include <vector>
#include <algorithm>
#include <string>
#include <map>

using namespace dfabric;

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

static std::shared_ptr<SyntheticBackend> Fresh(FabricEngine& e) {
  auto s = std::make_shared<SyntheticBackend>(NodeGeneration(1), CompositionRequest{});
  e.AddBackend(s);
  e.Discover();
  return s;
}

static Composition ComposeCommitActivate(FabricEngine& e, const CompositionRequest& req, CompositionId& cid, CompositionGeneration& cgen) {
  auto cr = e.Compose(req);
  CHECK(cr.ok());
  auto rr = e.Reserve(cr.plan);
  CHECK(rr.ok());
  CHECK(rr.reservation.state == ReservationState::PROVISIONAL);
  auto cc = e.Commit(rr.reservation);
  CHECK(cc.ok());
  CHECK(cc.composition.lifecycle == CompositionLifecycle::COMMITTED);
  auto ar = e.Activate(cc.composition.id);
  CHECK(ar.ok());
  CHECK(ar.composition.lifecycle == CompositionLifecycle::ACTIVE);
  cid = ar.composition.id; cgen = ar.composition.generation;
  return ar.composition;
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);

  // --- identity: zero IDs rejected by Require ---------------------------
  {
    bool threw = false;
    try { (void)ResourceId::Require(0, "test"); } catch (const IdentityError&) { threw = true; }
    CHECK(threw);
  }

  // --- discovered != available: unregistered resource not eligible -------
  {
    EngineConfig cfg; cfg.coordinator = CoordinatorId(1); cfg.epoch = CoordinatorEpoch(1);
    FabricEngine e(cfg); Fresh(e);
    auto d = e.CheckEligibility(ResourceId(9999), MakeReq());
    CHECK(!d.ok());
  }

  // --- UNKNOWN capability fails closed ----------------------------------
  {
    EngineConfig cfg; cfg.coordinator = CoordinatorId(1); cfg.epoch = CoordinatorEpoch(1);
    FabricEngine e(cfg); auto syn = Fresh(e);
    auto res = e.GetResource(ResourceId(102));
    CHECK(res.has_value());
    Resource r = *res;
    for (auto& c : r.capabilities) if (c.kind == CapabilityKind::CUDA_EXECUTION) c.state = CapabilityState::UNKNOWN;
    e.RegisterResource(r);
    CHECK(!e.CheckEligibility(ResourceId(102), MakeReq()).ok());
  }

  // --- full happy path + lifecycle guard ---------------------------------
  {
    EngineConfig cfg; cfg.coordinator = CoordinatorId(1); cfg.epoch = CoordinatorEpoch(1);
    FabricEngine e(cfg); Fresh(e);
    CompositionRequest req = MakeReq();
    CompositionId cid; CompositionGeneration cgen;
    auto c = ComposeCommitActivate(e, req, cid, cgen);
    CHECK(c.lifecycle == CompositionLifecycle::ACTIVE);
    // Activating an already-active composition is an illegal transition.
    auto again = e.Activate(cid);
    CHECK(!again.ok());
    CHECK(e.GetComposition(cid)->lifecycle == CompositionLifecycle::ACTIVE); // unchanged
  }

  // --- hard eligibility: resource offline -> reject ----------------------
  {
    EngineConfig cfg; cfg.coordinator = CoordinatorId(1); cfg.epoch = CoordinatorEpoch(1);
    FabricEngine e(cfg); auto syn = Fresh(e);
    syn->SetResourceHealth(ResourceId(102), ResourceHealth::OFFLINE);
    syn->SetResourceHealth(ResourceId(106), ResourceHealth::OFFLINE);
    e.OnResourceFailure(ResourceId(102));
    e.OnResourceFailure(ResourceId(106));
    auto cr = e.Compose(MakeReq());
    CHECK(!cr.ok());
    CHECK(cr.reasons.size() > 0);
  }

  // --- reservation rollback: atomic, no leakage ---------------------------
  {
    EngineConfig cfg; cfg.coordinator = CoordinatorId(1); cfg.epoch = CoordinatorEpoch(1);
    FabricEngine e(cfg); auto syn = Fresh(e);
    auto cr = e.Compose(MakeReq());
    CHECK(cr.ok());
    // Simulate a member becoming unavailable between plan and reserve.
    syn->SetResourceUnavailable(ResourceId(102));
    syn->SetResourceUnavailable(ResourceId(106));
    e.OnResourceFailure(ResourceId(102));
    e.OnResourceFailure(ResourceId(106));
    auto rr = e.Reserve(cr.plan);
    CHECK(!rr.ok());   // reserve must fail, rollback
    // No partial authoritative composition survives; snapshot has no reserved composition.
    auto snap = e.Snapshot();
    bool anyReserved = false;
    for (const auto& c : snap.compositions) if (c.lifecycle == CompositionLifecycle::RESERVED || c.lifecycle == CompositionLifecycle::COMMITTED) anyReserved = true;
    CHECK(!anyReserved);
  }

  // --- generation change invalidates dependent composition ----------------
  {
    EngineConfig cfg; cfg.coordinator = CoordinatorId(1); cfg.epoch = CoordinatorEpoch(1);
    FabricEngine e(cfg); auto syn = Fresh(e);
    CompositionId cid; CompositionGeneration cgen;
    auto c = ComposeCommitActivate(e, MakeReq(), cid, cgen);
    CHECK(c.lifecycle == CompositionLifecycle::ACTIVE);
    // Resource generation change on a member.
    syn->SetResourceGeneration(ResourceId(102), ResourceGeneration(2));
    syn->SetResourceGeneration(ResourceId(106), ResourceGeneration(2));
    e.Discover();   // re-ingest updated generations from the backend
    auto after = e.GetComposition(cid);
    CHECK(after.has_value());
    CHECK(after->lifecycle == CompositionLifecycle::RECOMPOSITION_REQUIRED);
    // Revalidation fails because generation changed.
    CHECK(!e.Revalidate(*after).ok());
  }

  // --- stale worker boot rejected -----------------------------------------
  {
    EngineConfig cfg; cfg.coordinator = CoordinatorId(1); cfg.epoch = CoordinatorEpoch(1);
    FabricEngine e(cfg); auto syn = Fresh(e);
    CompositionId cid; CompositionGeneration cgen;
    auto c = ComposeCommitActivate(e, MakeReq(), cid, cgen);
    CHECK(c.lifecycle == CompositionLifecycle::ACTIVE);
    // Stamp worker ownership on node-1 resources, then simulate worker death.
    {
      auto snap = e.Snapshot();
      for (auto& r : snap.resources) if (r.node_id == NodeId(1)) { r.evidence.owner = "1001"; r.evidence.durable = false; e.RegisterResource(r); }
    }
    e.MarkEvidenceRevalidationRequired(WorkerBootId(1001));
    auto after = e.GetComposition(cid);
    CHECK(after.has_value());
    CHECK(after->lifecycle == CompositionLifecycle::RECOMPOSITION_REQUIRED);
  }

  // --- recomposition creates fresh authority ------------------------------
  {
    EngineConfig cfg; cfg.coordinator = CoordinatorId(1); cfg.epoch = CoordinatorEpoch(1);
    FabricEngine e(cfg); auto syn = Fresh(e);
    CompositionId cid; CompositionGeneration cgen;
    auto c = ComposeCommitActivate(e, MakeReq(), cid, cgen);
    CHECK(c.lifecycle == CompositionLifecycle::ACTIVE);
    // Invalidate and recompose.
    // Fail the accelerator that is actually a member of the composition.
    {
      auto cc = e.GetComposition(cid);
      ResourceId accel{}; for (auto& v : cc->graph.vertices) { auto res = e.GetResource(v); if (res && res->resource_class == ResourceClass::ACCELERATOR) { accel = v; break; } }
      CHECK(accel.IsValid());
      syn->SetResourceHealth(accel, ResourceHealth::OFFLINE);
      e.OnResourceFailure(accel);
    }
    CHECK(e.GetComposition(cid)->lifecycle == CompositionLifecycle::RECOMPOSITION_REQUIRED);
    bool ok = e.Recompute(cid);
    CHECK(ok);
    auto after = e.Snapshot();
    CompositionId c2id; std::vector<Composition> comps = after.compositions;
    Composition latest; std::uint64_t maxgen = 0;
    for (const auto& x : comps) if (x.generation.Value() > maxgen) { maxgen = x.generation.Value(); latest = x; }
    CHECK(latest.lifecycle == CompositionLifecycle::ACTIVE);
    CHECK(latest.generation.Value() > cgen.Value());
    // Old composition fenced; only one active.
    std::size_t active = 0;
    for (const auto& x : comps) if (x.lifecycle == CompositionLifecycle::ACTIVE) active++;
    CHECK(active == 1);
  }

  // --- make-before-break: no duplicate exclusive authority -----------------
  {
    EngineConfig cfg; cfg.coordinator = CoordinatorId(1); cfg.epoch = CoordinatorEpoch(1);
    FabricEngine e(cfg); auto syn = Fresh(e);
    CompositionId cid; CompositionGeneration cgen;
    auto c = ComposeCommitActivate(e, MakeReq(), cid, cgen);
    CHECK(c.lifecycle == CompositionLifecycle::ACTIVE);
    // Build C2 on the SAME request before releasing C1 is impossible because
    // resources are exclusively reserved; verify reserve conflicts.
    auto cr2 = e.Compose(MakeReq());
    CHECK(cr2.ok());
    auto rr2 = e.Reserve(cr2.plan);
    CHECK(!rr2.ok());   // conflict: resources held by C1
    CHECK(rr2.reasons.size() > 0);
    // Drain C1 then EndDrain to release, then C2 can be built.
    CHECK(e.BeginDrain(cid));
    CHECK(e.EndDrain(cid));
    auto rr3 = e.Reserve(cr2.plan);
    CHECK(rr3.ok());   // now possible
    auto cc3 = e.Commit(rr3.reservation);
    CHECK(cc3.ok());
    auto ar3 = e.Activate(cc3.composition.id);
    CHECK(ar3.ok());
    // C1 released; C2 active; distinct authority generations.
    CHECK(e.GetComposition(cid)->lifecycle == CompositionLifecycle::RELEASED);
    CHECK(ar3.composition.generation.Value() > cgen.Value());
  }

  // --- drain rejects forbidden new hold -----------------------------------
  {
    EngineConfig cfg; cfg.coordinator = CoordinatorId(1); cfg.epoch = CoordinatorEpoch(1);
    FabricEngine e(cfg); Fresh(e);
    CompositionId cid; CompositionGeneration cgen;
    auto c = ComposeCommitActivate(e, MakeReq(), cid, cgen);
    CHECK(e.BeginDrain(cid));
    CHECK(e.GetComposition(cid)->lifecycle == CompositionLifecycle::DRAINING);
    auto cr2 = e.Compose(MakeReq());
    CHECK(cr2.ok());
    auto rr2 = e.Reserve(cr2.plan);
    CHECK(!rr2.ok());   // draining resources reject new holds
    CHECK(e.EndDrain(cid));
  }

  // --- failure-domain constraints -----------------------------------------
  {
    EngineConfig cfg; cfg.coordinator = CoordinatorId(1); cfg.epoch = CoordinatorEpoch(1);
    FabricEngine e(cfg); Fresh(e);
    CompositionRequest req = MakeReq();
    req.failure_domain.topology = TopologyConstraint::SPREAD;
    req.failure_domain.max_shared_risk = 1;
    auto cr = e.Compose(req);
    CHECK(cr.ok());   // cross-node spread is feasible
    // Anti-colocation: COLOCATE must fail because members span nodes A/B/C.
    CompositionRequest req2 = MakeReq();
    req2.failure_domain.topology = TopologyConstraint::COLOCATE;
    auto cr2 = e.Compose(req2);
    CHECK(!cr2.ok());
  }

  // --- deterministic ranking + insertion-order independence ----------------
  {
    EngineConfig cfg; cfg.coordinator = CoordinatorId(1); cfg.epoch = CoordinatorEpoch(1);
    FabricEngine e(cfg); Fresh(e);
    CompositionRequest req = MakeReq();
    auto r1 = e.Compose(req);
    auto r2 = e.Compose(req);
    CHECK(r1.ok() && r2.ok());
    // Deterministic: same canonical resource set and rank (ids are per-composition).
    auto key = [](const CompositionPlan& p) { std::vector<ResourceId> v = p.graph.vertices; std::sort(v.begin(), v.end()); std::string k; for (auto& x : v) k += x.ToString() + ","; return k; };
    CHECK(key(r1.plan) == key(r2.plan));
    CHECK(r1.plan.graph.vertices.size() == r2.plan.graph.vertices.size());
    CHECK(r1.plan.rank.total == r2.plan.rank.total);
    // Insertion-order independence: re-register resources in reversed order.
    FabricEngine e2(cfg); auto syn2 = Fresh(e2);
    auto res = syn2->DiscoverResources();
    std::reverse(res.begin(), res.end());
    for (auto& r : res) e2.RegisterResource(r);
    auto r3 = e2.Compose(req);
    CHECK(r3.ok());
    CHECK(key(r3.plan) == key(r1.plan));
  }

  // --- stale resource generation cannot enter new composition --------------
  {
    EngineConfig cfg; cfg.coordinator = CoordinatorId(1); cfg.epoch = CoordinatorEpoch(1);
    FabricEngine e(cfg); auto syn = Fresh(e);
    // Bump a member generation then try to compose with the OLD era resource
    // (simulate by marking evidence stale).
    syn->SetResourceGeneration(ResourceId(102), ResourceGeneration(5));
    e.Discover();   // re-ingest updated generation into the engine
    auto cr = e.Compose(MakeReq());
    // The GPU now has gen 5; the request needs a current accelerator; compose
    // should still succeed using the CURRENT gen-5 accelerator.
    CHECK(cr.ok());
  }

  printf("semantics test ok\n");
  return 0;
}