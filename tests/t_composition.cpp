#include "test_util.hpp"
#include "dfabric/engine.hpp"
#include "synthetic_backend.hpp"
#include <memory>

using namespace dfabric;

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  EngineConfig cfg; cfg.coordinator = CoordinatorId(7); cfg.epoch = CoordinatorEpoch(1);
  FabricEngine e(cfg);
  auto syn = std::make_shared<SyntheticBackend>(NodeGeneration(1), CompositionRequest{});
  e.AddBackend(syn);
  auto d = e.Discover();
  CHECK(d.nodes.size() == 4);
  CHECK(d.resources.size() == 7);

  CompositionRequest req;
  req.compute_units = 1;
  req.accelerator_count = 1;
  req.accelerator_memory_bytes = 1.0;
  req.host_memory_bytes = 1024;
  req.expanded_memory_bytes = 1024;
  req.storage_bytes = 1024;
  req.network_endpoint_count = 1;
  req.min_bandwidth_mbps = 1000;
  req.max_latency_us = 100;
  req.mandatory_classes.insert(ResourceClass::COMPUTE);
  req.mandatory_classes.insert(ResourceClass::ACCELERATOR);
  req.mandatory_classes.insert(ResourceClass::HOST_MEMORY);
  req.mandatory_classes.insert(ResourceClass::EXPANDED_MEMORY);
  req.mandatory_classes.insert(ResourceClass::STORAGE);
  req.mandatory_classes.insert(ResourceClass::NETWORK_ENDPOINT);

  auto cr = e.Compose(req);
  printf("compose outcome=%s\n", ToString(cr.outcome));
  CHECK(cr.ok());
  CHECK(cr.plan.graph.vertices.size() >= 6);

  auto rr = e.Reserve(cr.plan);
  printf("reserve outcome=%s\n", ToString(rr.outcome));
  CHECK(rr.ok());
  CHECK(rr.reservation.state == ReservationState::PROVISIONAL);

  auto cc = e.Commit(rr.reservation);
  printf("commit outcome=%s\n", ToString(cc.outcome));
  CHECK(cc.ok());

  auto ar = e.Activate(cc.composition.id);
  printf("activate outcome=%s\n", ToString(ar.outcome));
  CHECK(ar.ok());
  CHECK(ar.composition.lifecycle == CompositionLifecycle::ACTIVE);

  printf("composition test ok\n");
  return 0;
}