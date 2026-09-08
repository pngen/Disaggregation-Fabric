// Disaggregation Fabric — narrow inspection CLI.
// Commands: discover | nodes | resources | compose | explain | reserve |
//            commit | activate | drain | recompose | audit | inspect-state.
// Output distinguishes REAL / SYNTHETIC / UNSUPPORTED / UNKNOWN / REVALIDATION_REQUIRED.
#include "dfabric/engine.hpp"
#include "system_backend.hpp"
#include "synthetic_backend.hpp"
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

using namespace dfabric;

static void PrintMatrix(const Resource& r) {
  const char* kind = r.synthetic ? "SYNTHETIC" : "REAL";
  std::printf("  %s %-24s class=%-16s node=%s gen=%llu", kind, r.id.ToString().c_str(), ToString(r.resource_class), r.node_id.ToString().c_str(), (unsigned long long)r.generation.Value());
  if (r.evidence.state == EvidenceState::REVALIDATION_REQUIRED) std::printf(" evidence=REVALIDATION_REQUIRED");
  else if (!r.evidence.IsCurrent()) std::printf(" evidence=STALE");
  else std::printf(" evidence=FRESH");
  std::printf(" health=%s\n", ToString(r.health));
}

int main(int argc, char** argv) {
  std::string cmd = (argc > 1) ? argv[1] : "discover";
  std::string backend = (argc > 2) ? argv[2] : "system";
  std::setvbuf(stdout, nullptr, _IONBF, 0); std::setvbuf(stderr, nullptr, _IONBF, 0);

  EngineConfig cfg; cfg.coordinator = CoordinatorId(1); cfg.epoch = CoordinatorEpoch(1);
  FabricEngine e(cfg);
  BackendPtr b;
  if (backend == "synthetic") b = std::make_shared<SyntheticBackend>(NodeGeneration(1), CompositionRequest{});
  else b = std::make_shared<SystemBackend>();
  e.AddBackend(b);
  e.Discover();

  if (cmd == "discover" || cmd == "nodes" || cmd == "resources") {
    auto snap = e.Snapshot();
    std::printf("backend=%s kind=%s (multi-node=%s)\n", b->name().c_str(), ToString(b->kind()), b->kind()==BackendKind::REAL ? "single-host only" : "synthetic");
    for (auto& n : snap.nodes) {
      std::printf("node %s hostname=%s gen=%llu health=%s reach=%s domain=%s backend=%s\n",
                  n.id.ToString().c_str(), n.hostname.c_str(), (unsigned long long)n.generation.Value(),
                  ToString(n.health), ToString(n.reachability), n.failure_domain.name.c_str(), ToString(n.backend));
    }
    if (cmd == "resources" || cmd == "discover") for (auto& r : snap.resources) PrintMatrix(r);
    std::printf("totals: %zu nodes %zu resources\n", snap.nodes.size(), snap.resources.size());
    return 0;
  }
  if (cmd == "compose") {
    CompositionRequest req; req.compute_units = 1; req.accelerator_count = 1; req.accelerator_memory_bytes = 1.0;
    req.host_memory_bytes = 1024; req.expanded_memory_bytes = 1024; req.storage_bytes = 1024; req.network_endpoint_count = 1;
    req.min_bandwidth_mbps = 1000; req.max_latency_us = 100;
    req.mandatory_classes.insert(ResourceClass::COMPUTE); req.mandatory_classes.insert(ResourceClass::ACCELERATOR);
    req.mandatory_classes.insert(ResourceClass::HOST_MEMORY); req.mandatory_classes.insert(ResourceClass::EXPANDED_MEMORY);
    req.mandatory_classes.insert(ResourceClass::STORAGE); req.mandatory_classes.insert(ResourceClass::NETWORK_ENDPOINT);
    auto cr = e.Compose(req);
    std::printf("compose outcome=%s score=%f vertices=%zu reasons=", ToString(cr.outcome), cr.plan.rank.total, cr.plan.graph.vertices.size());
    for (auto r : cr.reasons) std::printf("%s ", ToString(r));
    std::printf("\n");
    for (auto& v : cr.plan.graph.vertices) std::printf("  vertex %s\n", v.ToString().c_str());
    return 0;
  }
  if (cmd == "explain") {
    auto snap = e.Snapshot();
    for (auto& r : snap.resources) std::printf("%s\n", e.ExplainEligibility(r.id, CompositionRequest{}).c_str());
    return 0;
  }
  if (cmd == "inspect-state" || cmd == "audit") {
    auto snap = e.Snapshot();
    std::printf("epoch=%llu compositions=%zu reservations=%zu\n", (unsigned long long)snap.epoch.Value(), snap.compositions.size(), snap.reservations.size());
    for (auto& c : snap.compositions) std::printf("  composition %s gen=%llu lifecycle=%s outcome=%s\n", c.id.ToString().c_str(), (unsigned long long)c.generation.Value(), ToString(c.lifecycle), ToString(c.outcome));
    for (auto& rv : snap.reservations) std::printf("  reservation %s state=%s items=%zu\n", rv.id.ToString().c_str(), ToString(rv.state), rv.items.size());
    return 0;
  }
  std::printf("unknown command: %s (use discover/nodes/resources/compose/explain/inspect-state/audit)\n", cmd.c_str());
  return 1;
}
