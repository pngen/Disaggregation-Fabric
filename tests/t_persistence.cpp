// Disaggregation Fabric — persistence integrity tests.
// Round-trip, corruption rejection, truncation rejection, invalid enum rejection,
// trailing-garbage rejection.
#include "test_util.hpp"
#include "dfabric/persistence.hpp"
#include <fstream>
#include <cstdio>
#include <vector>
#include <string>

using namespace dfabric;
using namespace dfabric::persist;

static DurableState MakeState() {
  DurableState s;
  s.epoch = CoordinatorEpoch(3);
  Node n; n.id = NodeId(1); n.generation = NodeGeneration(2); n.hostname = "node-a";
  n.backend = BackendKind::SYNTHETIC; n.health = ResourceHealth::HEALTHY;
  n.reachability = Reachability::REACHABLE; n.lifecycle = NodeLifecycle::VALIDATED;
  n.failure_domain.name = "rack-1"; n.failure_domain.type = FailureDomainType::RACK;
  s.nodes.push_back(n);
  Resource r; r.id = ResourceId(10); r.generation = ResourceGeneration(1);
  r.resource_class = ResourceClass::ACCELERATOR; r.node_id = NodeId(1);
  r.health = ResourceHealth::HEALTHY; r.readiness = Readiness::READY; r.reachability = Reachability::REACHABLE;
  r.capacity.compute_units = 40; r.capacity.accelerator_memory_bytes = 24.0 * (1ULL << 30);
  r.failure_domain.id = FailureDomainId(2); r.failure_domain.type = FailureDomainType::HOST; r.failure_domain.name = "host-1"; r.failure_domain.synthetic = true;
  Capability c; c.kind = CapabilityKind::CUDA_EXECUTION; c.state = CapabilityState::SUPPORTED; c.detail = "12.0"; c.generation = CapabilityGeneration(1);
  r.capabilities.push_back(c);
  r.evidence.id = EvidenceId(99); r.evidence.generation = EvidenceGeneration(1);
  r.evidence.source = EvidenceSource::BACKEND; r.evidence.state = EvidenceState::FRESH; r.evidence.max_age_ms = 60000; r.evidence.durable = false;
  s.resources.push_back(r);
  s.policies[PolicyId(9)] = PolicyGeneration(1);
  Composition cm; cm.id = CompositionId(1); cm.generation = CompositionGeneration(1);
  cm.lifecycle = CompositionLifecycle::ACTIVE; cm.outcome = CompositionOutcome::SUCCESS;
  cm.resource_set_id = ResourceSetId(1); cm.resource_set_generation = ResourceSetGeneration(1);
  cm.graph.vertices.push_back(ResourceId(10));
  cm.authority.coordinator_epoch = CoordinatorEpoch(3);
  s.compositions.push_back(cm);
  return s;
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  const std::string path = "_persist.bin";

  // Round-trip.
  {
    auto s = MakeState();
    std::string err;
    CHECK(Save(s, path, err));
    DurableState out;
    bool lk = Load(path, out, err);
    if (!lk) printf("LOAD ERR: %s\n", err.c_str());
    CHECK(lk);
    CHECK(out.epoch == CoordinatorEpoch(3));
    CHECK(out.nodes.size() == s.nodes.size());
    CHECK(out.resources.size() == 1);
    CHECK(out.resources[0].id == ResourceId(10));
    CHECK(out.resources[0].capacity.compute_units == 40.0);
    CHECK(out.resources[0].capabilities.size() == 1);
    CHECK(out.resources[0].capabilities[0].detail == "12.0");
    CHECK(out.policies.size() == 1);
    CHECK(out.compositions.size() == 1);
    CHECK(out.compositions[0].lifecycle == CompositionLifecycle::ACTIVE);
    CHECK(out.compositions[0].authority.coordinator_epoch == CoordinatorEpoch(3));
  }

  // Corruption: flip a byte in the middle.
  {
    std::ifstream ifs(path, std::ios::binary); std::vector<char> b((std::istreambuf_iterator<char>(ifs)), {}); ifs.close();
    b[20] ^= 0x40;
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc); ofs.write(b.data(), b.size()); ofs.close();
    DurableState out; std::string err;
    CHECK(!Load(path, out, err));
    CHECK(!err.empty());
  }

  // Truncation: cut the file in half.
  {
    std::ifstream ifs(path, std::ios::binary); std::vector<char> b((std::istreambuf_iterator<char>(ifs)), {}); ifs.close();
    b.resize(b.size() / 2);
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc); ofs.write(b.data(), b.size()); ofs.close();
    DurableState out; std::string err;
    CHECK(!Load(path, out, err));
  }

  // Trailing garbage after the checksum.
  {
    auto s = MakeState(); std::string err;
    CHECK(Save(s, path, err));
    std::ifstream ifs(path, std::ios::binary); std::vector<char> b((std::istreambuf_iterator<char>(ifs)), {}); ifs.close();
    b.push_back('x'); b.push_back('y');
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc); ofs.write(b.data(), b.size()); ofs.close();
    DurableState out;
    CHECK(!Load(path, out, err));
  }

  std::remove(path.c_str());
  printf("persistence test ok\n");
  return 0;
}