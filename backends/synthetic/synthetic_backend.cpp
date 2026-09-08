// Disaggregation Fabric — synthetic multi-node backend implementation.
#include "synthetic_backend.hpp"
#include <sstream>
#include <algorithm>

namespace dfabric {

namespace {
Resource MakeBase(ResourceId id, ResourceGeneration gen, ResourceClass cls, NodeId node,
                  const std::string& provider) {
  Resource r;
  r.id = id; r.generation = gen; r.resource_class = cls; r.node_id = node;
  r.provider = provider;
  r.health = ResourceHealth::HEALTHY;
  r.readiness = Readiness::READY;
  r.reachability = Reachability::REACHABLE;
  r.evidence.id = EvidenceId(id.Value() * 1000 + 1);
  r.evidence.generation = EvidenceGeneration(1);
  r.evidence.source = EvidenceSource::BACKEND;
  r.evidence.state = EvidenceState::FRESH;
  r.evidence.timestamp_ms = 1000;
  r.evidence.max_age_ms = 3600000;
  r.evidence.confidence = 1.0;
  r.evidence.provenance = provider + "/synthetic";
  r.evidence.durable = true;
  r.synthetic = true;
  r.failure_domain.id = FailureDomainId(node.Value() * 10 + 1);
  r.failure_domain.type = FailureDomainType::HOST;
  r.failure_domain.name = "host-" + node.ToString();
  r.failure_domain.synthetic = true;
  return r;
}

void AddCap(Resource& r, CapabilityKind kind, CapabilityState state, const std::string& detail) {
  Capability c; c.kind = kind; c.state = state; c.generation = CapabilityGeneration(1);
  c.evidence = r.evidence.id; c.detail = detail;
  r.capabilities.push_back(c);
}
}  // namespace

SyntheticBackend::SyntheticBackend(NodeGeneration ng, CompositionRequest) : node_gen_(ng) {
  std::lock_guard<std::mutex> lk(mu_);

  Node a; a.id = NodeId(1); a.generation = ng; a.hostname = "node-a"; a.provider = "synthetic";
  a.backend = BackendKind::SYNTHETIC; a.health = ResourceHealth::HEALTHY; a.reachability = Reachability::REACHABLE;
  a.failure_domain.id = FailureDomainId(1); a.failure_domain.type = FailureDomainType::HOST; a.failure_domain.name = "rack-1"; a.failure_domain.synthetic = true;
  a.lifecycle = NodeLifecycle::VALIDATED; a.synthetic = true;
  Node b = a; b.id = NodeId(2); b.hostname = "node-b"; b.failure_domain.id = FailureDomainId(2); b.failure_domain.name = "rack-2";
  Node c = a; c.id = NodeId(3); c.hostname = "node-c"; c.failure_domain.id = FailureDomainId(3); c.failure_domain.name = "rack-3";
  Node d = a; d.id = NodeId(4); d.hostname = "node-d"; d.failure_domain.id = FailureDomainId(4); d.failure_domain.name = "rack-4";
  nodes_ = { a, b, c, d };

  // Node A: compute + accelerator.
  Resource cpu = MakeBase(ResourceId(101), ResourceGeneration(1), ResourceClass::COMPUTE, NodeId(1), "synthetic");
  cpu.capacity.compute_units = 32; cpu.capacity.host_memory_bytes = 64.0 * (1ULL << 30); cpu.capacity.network_latency_us = 1;
  AddCap(cpu, CapabilityKind::CPU_EXECUTION, CapabilityState::SUPPORTED, "");
  AddCap(cpu, CapabilityKind::HOST_MEMORY_ACCESS, CapabilityState::SUPPORTED, "");
  AddCap(cpu, CapabilityKind::COHERENT_MEMORY_ACCESS, CapabilityState::SUPPORTED, "");
  AddCap(cpu, CapabilityKind::PERSISTENT_STORAGE, CapabilityState::SUPPORTED, "");

  Resource gpu = MakeBase(ResourceId(102), ResourceGeneration(1), ResourceClass::ACCELERATOR, NodeId(1), "synthetic");
  gpu.capacity.compute_units = 40; gpu.capacity.accelerator_memory_bytes = 24.0 * (1ULL << 30);
  AddCap(gpu, CapabilityKind::CUDA_EXECUTION, CapabilityState::SUPPORTED, "12.0");
  AddCap(gpu, CapabilityKind::ARCH_GENERATION, CapabilityState::SUPPORTED, "12.0");
  AddCap(gpu, CapabilityKind::COMPUTE_PRECISION, CapabilityState::SUPPORTED, "fp64");

  // Node A: host memory.
  Resource hm = MakeBase(ResourceId(107), ResourceGeneration(1), ResourceClass::HOST_MEMORY, NodeId(1), "synthetic");
  hm.capacity.host_memory_bytes = 64.0 * (1ULL << 30);
  AddCap(hm, CapabilityKind::HOST_MEMORY_ACCESS, CapabilityState::SUPPORTED, "");

  // Node B: expanded memory.
  Resource mem = MakeBase(ResourceId(103), ResourceGeneration(1), ResourceClass::EXPANDED_MEMORY, NodeId(2), "synthetic");
  mem.capacity.expanded_memory_bytes = 256.0 * (1ULL << 30);
  AddCap(mem, CapabilityKind::EXPANDED_MEMORY_ACCESS, CapabilityState::SUPPORTED, "");
  AddCap(mem, CapabilityKind::HOST_MEMORY_ACCESS, CapabilityState::SUPPORTED, "");

  // Node C: storage + NIC.
  Resource stor = MakeBase(ResourceId(104), ResourceGeneration(1), ResourceClass::STORAGE, NodeId(3), "synthetic");
  stor.capacity.storage_bytes = 2.0 * (1ULL << 40); stor.capacity.network_bandwidth_mbps = 50000;
  AddCap(stor, CapabilityKind::PERSISTENT_STORAGE, CapabilityState::SUPPORTED, "");
  Resource nic = MakeBase(ResourceId(105), ResourceGeneration(1), ResourceClass::NETWORK_ENDPOINT, NodeId(3), "synthetic");
  nic.capacity.network_bandwidth_mbps = 100000;
  AddCap(nic, CapabilityKind::DIRECT_NETWORK_ACCESS, CapabilityState::SUPPORTED, "");
  AddCap(nic, CapabilityKind::NETWORK_PROTOCOL, CapabilityState::SUPPORTED, "tcp");

  // Node D: replacement accelerator.
  Resource gpu2 = MakeBase(ResourceId(106), ResourceGeneration(1), ResourceClass::ACCELERATOR, NodeId(4), "synthetic");
  gpu2.capacity.compute_units = 40; gpu2.capacity.accelerator_memory_bytes = 24.0 * (1ULL << 30);
  AddCap(gpu2, CapabilityKind::CUDA_EXECUTION, CapabilityState::SUPPORTED, "12.0");
  AddCap(gpu2, CapabilityKind::ARCH_GENERATION, CapabilityState::SUPPORTED, "12.0");
  AddCap(gpu2, CapabilityKind::COMPUTE_PRECISION, CapabilityState::SUPPORTED, "fp64");

  resources_ = { cpu, gpu, hm, mem, stor, nic, gpu2 };

  auto addPath = [&](NodeId f, NodeId t, double bw, double lat) {
    PathState p{ true, bw, lat, PathGeneration(1) };
    paths_[f.ToString() + "->" + t.ToString()] = p;
    paths_[t.ToString() + "->" + f.ToString()] = p;
  };
  addPath(NodeId(1), NodeId(2), 100000, 3);
  addPath(NodeId(1), NodeId(3), 50000, 5);
  addPath(NodeId(2), NodeId(3), 50000, 8);
  addPath(NodeId(1), NodeId(4), 100000, 2);
  addPath(NodeId(4), NodeId(2), 100000, 4);
  addPath(NodeId(4), NodeId(3), 50000, 6);
}

std::vector<Node> SyntheticBackend::DiscoverNodes() {
  std::lock_guard<std::mutex> lk(mu_);
  return nodes_;
}

std::vector<Resource> SyntheticBackend::DiscoverResources() {
  std::lock_guard<std::mutex> lk(mu_);
  return resources_;
}

std::optional<PathEvidence> SyntheticBackend::ResolvePath(const PathRequirement& q) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = paths_.find(q.from.ToString() + "->" + q.to.ToString());
  if (it == paths_.end()) return std::nullopt;
  const PathState& p = it->second;
  if (!p.available) return std::nullopt;
  PathEvidence pe;
  pe.id = PathEvidenceId(q.from.Value() * 100 + q.to.Value());
  pe.generation = p.gen;
  pe.requirement = q;
  pe.available = true; pe.verified = true;
  pe.bandwidth_mbps = p.bw; pe.latency_us = p.lat;
  pe.provider = "synthetic"; pe.source = "synthetic";
  pe.evidence.id = EvidenceId(pe.id.Value()); pe.evidence.generation = EvidenceGeneration(1);
  pe.evidence.source = EvidenceSource::BACKEND; pe.evidence.state = EvidenceState::FRESH;
  pe.evidence.timestamp_ms = 1000; pe.evidence.max_age_ms = 3600000; pe.evidence.confidence = 1.0;
  pe.evidence.durable = true;
  return pe;
}

std::string SyntheticBackend::Describe() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::ostringstream os;
  os << "synthetic nodes=" << nodes_.size() << " resources=" << resources_.size();
  return os.str();
}

void SyntheticBackend::SetResourceHealth(ResourceId id, ResourceHealth h) {
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& r : resources_) if (r.id == id) { r.health = h; r.evidence.state = (h == ResourceHealth::HEALTHY ? EvidenceState::FRESH : EvidenceState::REVALIDATION_REQUIRED); }
}
void SyntheticBackend::SetResourceReachability(ResourceId id, Reachability rc) {
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& r : resources_) if (r.id == id) { r.reachability = rc; r.evidence.state = (rc == Reachability::REACHABLE ? EvidenceState::FRESH : EvidenceState::REVALIDATION_REQUIRED); }
}
void SyntheticBackend::SetResourceGeneration(ResourceId id, ResourceGeneration g) {
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& r : resources_) if (r.id == id) { r.generation = g; r.evidence.generation = EvidenceGeneration(g.Value()); r.evidence.state = EvidenceState::FRESH; r.evidence.timestamp_ms += 1; }
}
void SyntheticBackend::SetResourceUnavailable(ResourceId id) {
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& r : resources_) if (r.id == id) { r.health = ResourceHealth::OFFLINE; r.reachability = Reachability::UNREACHABLE; r.readiness = Readiness::NOT_READY; r.evidence.state = EvidenceState::REVALIDATION_REQUIRED; r.evidence.durable = false; }
}
void SyntheticBackend::SetResourceBack(ResourceId id) {
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& r : resources_) if (r.id == id) { r.health = ResourceHealth::HEALTHY; r.reachability = Reachability::REACHABLE; r.readiness = Readiness::READY; r.evidence.state = EvidenceState::FRESH; r.evidence.max_age_ms = 3600000; r.evidence.durable = true; }
}
void SyntheticBackend::SetNodeHealth(NodeId id, ResourceHealth h) {
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& n : nodes_) if (n.id == id) n.health = h;
}
void SyntheticBackend::SetPathAvailable(NodeId f, NodeId t, bool available) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = paths_.find(f.ToString() + "->" + t.ToString());
  if (it != paths_.end()) { it->second.available = available; it->second.gen = it->second.gen.Next(); }
}
void SyntheticBackend::SetPathBandwidth(NodeId f, NodeId t, double mbps) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = paths_.find(f.ToString() + "->" + t.ToString());
  if (it != paths_.end()) { it->second.bw = mbps; it->second.gen = it->second.gen.Next(); }
}
void SyntheticBackend::SetPathLatency(NodeId f, NodeId t, double us) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = paths_.find(f.ToString() + "->" + t.ToString());
  if (it != paths_.end()) { it->second.lat = us; it->second.gen = it->second.gen.Next(); }
}
void SyntheticBackend::RemoveNode(NodeId id) {
  std::lock_guard<std::mutex> lk(mu_);
  nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(), [&](const Node& n){ return n.id == id; }), nodes_.end());
  for (auto& r : resources_) if (r.node_id == id) { r.health = ResourceHealth::OFFLINE; r.reachability = Reachability::UNREACHABLE; r.evidence.state = EvidenceState::REVALIDATION_REQUIRED; }
}
void SyntheticBackend::ReincarnateNode(NodeId id, NodeGeneration gen, WorkerBootId boot) {
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& n : nodes_) if (n.id == id) { n.generation = gen; n.worker_boot = boot; n.health = ResourceHealth::HEALTHY; }
  for (auto& r : resources_) if (r.node_id == id) { r.generation = ResourceGeneration(gen.Value()); r.health = ResourceHealth::HEALTHY; r.reachability = Reachability::REACHABLE; r.readiness = Readiness::READY; r.evidence.owner = boot.ToString(); r.evidence.state = EvidenceState::FRESH; r.evidence.durable = false; r.evidence.max_age_ms = 3600000; }
}
std::string SyntheticBackend::Dump() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::ostringstream os;
  for (const auto& n : nodes_) os << "node " << n.id.ToString() << " " << n.hostname << " gen=" << n.generation.Value() << "\n";
  for (const auto& r : resources_) os << "res " << r.id.ToString() << " cls=" << ToString(r.resource_class) << " gen=" << r.generation.Value() << " health=" << ToString(r.health) << "\n";
  return os.str();
}

}  // namespace dfabric