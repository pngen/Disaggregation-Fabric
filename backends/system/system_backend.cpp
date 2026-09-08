// Disaggregation Fabric — real single-host system backend implementation.
#include "system_backend.hpp"
#include <windows.h>
#include <cstdio>

namespace dfabric {

namespace {
std::string GetHostName() {
  char buf[256]; DWORD sz = sizeof(buf);
  if (GetComputerNameA(buf, &sz)) return std::string(buf, sz);
  return "localhost";
}
}

SystemBackend::SystemBackend() {
  SYSTEM_INFO si{}; GetSystemInfo(&si);
  MEMORYSTATUSEX ms{}; ms.dwLength = sizeof(ms); GlobalMemoryStatusEx(&ms);
  ULONGLONG freeBytes = 0, totalBytes = 0, totalFree = 0;
  GetDiskFreeSpaceExA("C:\\", (PULARGE_INTEGER)&freeBytes, (PULARGE_INTEGER)&totalBytes, (PULARGE_INTEGER)&totalFree);

  node_.id = NodeId(9000);
  node_.generation = NodeGeneration(1);
  node_.hostname = GetHostName();
  node_.provider = "windows/host";
  node_.backend = BackendKind::REAL;
  node_.health = ResourceHealth::HEALTHY;
  node_.reachability = Reachability::REACHABLE;
  node_.lifecycle = NodeLifecycle::VALIDATED;
  node_.failure_domain.id = FailureDomainId(9001);
  node_.failure_domain.type = FailureDomainType::HOST;
  node_.failure_domain.name = "single-host";
  node_.failure_domain.synthetic = false;
  node_.synthetic = false;

  auto mk = [&](ResourceId id, ResourceClass cls, const std::string& prov) {
    Resource r;
    r.id = id; r.generation = ResourceGeneration(1); r.resource_class = cls; r.node_id = node_.id;
    r.provider = prov; r.health = ResourceHealth::HEALTHY; r.readiness = Readiness::READY; r.reachability = Reachability::REACHABLE;
    r.evidence.id = EvidenceId(id.Value() * 7 + 1); r.evidence.generation = EvidenceGeneration(1);
    r.evidence.source = EvidenceSource::BACKEND; r.evidence.state = EvidenceState::FRESH;
    r.evidence.timestamp_ms = 1000; r.evidence.max_age_ms = 3600000; r.evidence.confidence = 0.9;
    r.evidence.provenance = "system/" + prov; r.evidence.durable = true; r.synthetic = false;
    r.failure_domain = node_.failure_domain;
    return r;
  };

  Resource cpu = mk(ResourceId(9101), ResourceClass::COMPUTE, "host/cpu");
  cpu.capacity.compute_units = static_cast<double>(si.dwNumberOfProcessors);
  Capability c1; c1.kind = CapabilityKind::CPU_EXECUTION; c1.state = CapabilityState::SUPPORTED; c1.generation = CapabilityGeneration(1); c1.evidence = cpu.evidence.id;
  cpu.capabilities.push_back(c1);
  Capability c2; c2.kind = CapabilityKind::HOST_MEMORY_ACCESS; c2.state = CapabilityState::SUPPORTED; c2.generation = CapabilityGeneration(1); c2.evidence = cpu.evidence.id;
  cpu.capabilities.push_back(c2);

  Resource mem = mk(ResourceId(9102), ResourceClass::HOST_MEMORY, "host/memory");
  mem.capacity.host_memory_bytes = static_cast<double>(ms.ullTotalPhys);
  Capability m1; m1.kind = CapabilityKind::HOST_MEMORY_ACCESS; m1.state = CapabilityState::SUPPORTED; m1.generation = CapabilityGeneration(1); m1.evidence = mem.evidence.id;
  mem.capabilities.push_back(m1);

  Resource stor = mk(ResourceId(9103), ResourceClass::STORAGE, "host/storage");
  stor.capacity.storage_bytes = static_cast<double>(freeBytes);
  Capability s1; s1.kind = CapabilityKind::PERSISTENT_STORAGE; s1.state = CapabilityState::SUPPORTED; s1.generation = CapabilityGeneration(1); s1.evidence = stor.evidence.id;
  stor.capabilities.push_back(s1);

  Resource nic = mk(ResourceId(9104), ResourceClass::NETWORK_ENDPOINT, "host/network");
  nic.capacity.network_bandwidth_mbps = 1000.0;   // loopback conservatively
  Capability n1; n1.kind = CapabilityKind::DIRECT_NETWORK_ACCESS; n1.state = CapabilityState::SUPPORTED; n1.generation = CapabilityGeneration(1); n1.evidence = nic.evidence.id;
  nic.capabilities.push_back(n1);

  res_ = { cpu, mem, stor, nic };
}

std::vector<Node> SystemBackend::DiscoverNodes() { return { node_ }; }
std::vector<Resource> SystemBackend::DiscoverResources() { return res_; }
std::optional<PathEvidence> SystemBackend::ResolvePath(const PathRequirement&) {
  // Single-host: cross-node paths are not applicable (one physical node).
  return std::nullopt;
}
std::string SystemBackend::Describe() const {
  char buf[256];
  snprintf(buf, sizeof(buf), "system host=%s resources=%zu", node_.hostname.c_str(), res_.size());
  return buf;
}

}
