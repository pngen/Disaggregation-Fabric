// Independent downstream consumer: consumes the installed DisaggregationFabric
// package via find_package, registers synthetic resources, composes a valid
// resource set, activates it, and exits 0.
#include "dfabric/engine.hpp"
#include "dfabric/backends/synthetic/synthetic_backend.hpp"
#include <cstdio>
#include <memory>

using namespace dfabric;

int main() {
  EngineConfig cfg; cfg.coordinator = CoordinatorId(42); cfg.epoch = CoordinatorEpoch(1);
  FabricEngine e(cfg);
  auto syn = std::make_shared<SyntheticBackend>(NodeGeneration(1), CompositionRequest{});
  e.AddBackend(syn);
  e.Discover();

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

  auto cr = e.Compose(req);
  if (!cr.ok()) { std::printf("consumer: compose failed\n"); return 1; }
  auto rr = e.Reserve(cr.plan);
  if (!rr.ok()) { std::printf("consumer: reserve failed\n"); return 1; }
  auto cc = e.Commit(rr.reservation);
  if (!cc.ok()) { std::printf("consumer: commit failed\n"); return 1; }
  auto ar = e.Activate(cc.composition.id);
  if (!ar.ok()) { std::printf("consumer: activate failed\n"); return 1; }
  if (ar.composition.lifecycle != CompositionLifecycle::ACTIVE) { std::printf("consumer: not active\n"); return 1; }
  std::printf("consumer: find_package + compose/reserve/commit/activate OK (composition id=%llu gen=%llu)\n",
              (unsigned long long)ar.composition.id.Value(), (unsigned long long)ar.composition.generation.Value());
  return 0;
}