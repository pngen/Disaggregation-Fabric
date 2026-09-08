// Disaggregation Fabric — persistence codec (versioned, integrity-checked).
#include "dfabric/persistence.hpp"
#include <fstream>
#include <cstring>
#include <vector>
#include <cstdint>

namespace dfabric {
namespace persist {

namespace {

constexpr std::uint32_t kMagic = 0x46414244;        // "DFAB" little-endian marker
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::uint32_t kMaxCount = 1u << 20;       // bounded counts
constexpr std::uint32_t kMaxLen = 1u << 15;         // bounded string length

// FNV-1a 64-bit.
std::uint64_t Checksum(const std::uint8_t* data, std::size_t n) {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (std::size_t i = 0; i < n; ++i) {
    h ^= static_cast<std::uint64_t>(data[i]);
    h *= 0x100000001b3ULL;
  }
  return h;
}

struct Writer {
  std::vector<std::uint8_t> buf;
  void U8(std::uint8_t v) { buf.push_back(v); }
  void U16(std::uint16_t v) { buf.push_back(v & 0xff); buf.push_back((v >> 8) & 0xff); }
  void U32(std::uint32_t v) { for (int i = 0; i < 4; ++i) buf.push_back((v >> (8 * i)) & 0xff); }
  void U64(std::uint64_t v) { for (int i = 0; i < 8; ++i) buf.push_back((v >> (8 * i)) & 0xff); }
  void Dbl(double d) { std::uint64_t u; std::memcpy(&u, &d, sizeof(u)); U64(u); }
  void Str(const std::string& s) {
    if (s.size() > kMaxLen) throw std::runtime_error("persist: string too long");
    U32(static_cast<std::uint32_t>(s.size()));
    buf.insert(buf.end(), s.begin(), s.end());
  }
};

struct Reader {
  const std::uint8_t* data;
  std::size_t size;
  std::size_t pos = 0;
  bool ok = true;
  explicit Reader(const std::vector<std::uint8_t>& v) : data(v.data()), size(v.size()) {}
  bool Need(std::size_t n) { return ok && pos + n <= size && pos + n >= pos; }
  std::uint8_t U8() { if (!Need(1)) { ok = false; return 0; } return data[pos++]; }
  std::uint16_t U16() { if (!Need(2)) { ok = false; return 0; } std::uint16_t v = static_cast<std::uint16_t>(data[pos]) | (static_cast<std::uint16_t>(data[pos + 1]) << 8); pos += 2; return v; }
  std::uint32_t U32() { if (!Need(4)) { ok = false; return 0; } std::uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(data[pos + i]) << (8 * i); pos += 4; return v; }
  std::uint64_t U64() { if (!Need(8)) { ok = false; return 0; } std::uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(data[pos + i]) << (8 * i); pos += 8; return v; }
  double Dbl() { std::uint64_t u = U64(); double d; std::memcpy(&d, &u, sizeof(d)); return d; }
  std::string Str() { std::uint32_t n = U32(); if (!ok || n > kMaxLen || !Need(n)) { ok = false; return {}; } std::string s(reinterpret_cast<const char*>(data + pos), n); pos += n; return s; }
};

// Enum validation helpers: reject values outside the canonical table.
template <typename E>
bool ValidEnum(std::uint32_t v) {
  return v >= static_cast<std::uint32_t>(EnumTraits<E>::Min()) && v <= EnumTraits<E>::MaxValue;
}

void WriteResource(Writer& w, const Resource& r) {
  w.U64(r.id.Value()); w.U64(r.generation.Value());
  w.U32(static_cast<std::uint32_t>(r.resource_class));
  w.U64(r.node_id.Value());
  w.Str(r.provider);
  w.Str(r.authority_owner);
  w.U8(r.synthetic ? 1 : 0);
  w.U32(static_cast<std::uint32_t>(r.health));
  w.U32(static_cast<std::uint32_t>(r.readiness));
  w.U32(static_cast<std::uint32_t>(r.reachability));
  // capacity
  w.Dbl(r.capacity.compute_units); w.Dbl(r.capacity.accelerator_memory_bytes);
  w.Dbl(r.capacity.host_memory_bytes); w.Dbl(r.capacity.expanded_memory_bytes);
  w.Dbl(r.capacity.storage_bytes); w.Dbl(r.capacity.network_bandwidth_mbps);
  w.Dbl(r.capacity.network_latency_us);
  if (r.capacity.extra.size() > kMaxCount) throw std::runtime_error("persist: extra too large");
  w.U32(static_cast<std::uint32_t>(r.capacity.extra.size()));
  for (const auto& kv : r.capacity.extra) { w.Str(kv.first); w.Dbl(kv.second); }
  // locality
  w.U32(static_cast<std::uint32_t>(r.locality.coords.size()));
  for (double c : r.locality.coords) w.Dbl(c);
  w.U32(static_cast<std::uint32_t>(r.locality.tags.size()));
  for (const auto& kv : r.locality.tags) { w.Str(kv.first); w.Dbl(kv.second); }
  // failure domain
  w.U64(r.failure_domain.id.Value());
  w.U32(static_cast<std::uint32_t>(r.failure_domain.type));
  w.Str(r.failure_domain.name);
  w.U8(r.failure_domain.synthetic ? 1 : 0);
  // evidence
  w.U64(r.evidence.id.Value()); w.U64(r.evidence.generation.Value());
  w.U32(static_cast<std::uint32_t>(r.evidence.source));
  w.U32(static_cast<std::uint32_t>(r.evidence.state));
  w.U64(r.evidence.timestamp_ms); w.U64(r.evidence.max_age_ms);
  w.Dbl(r.evidence.confidence); w.Str(r.evidence.provenance); w.Str(r.evidence.owner);
  w.U8(r.evidence.durable ? 1 : 0);
  // capabilities
  w.U32(static_cast<std::uint32_t>(r.capabilities.size()));
  for (const auto& c : r.capabilities) {
    w.U32(static_cast<std::uint32_t>(c.kind)); w.U32(static_cast<std::uint32_t>(c.state));
    w.U64(c.generation.Value()); w.U64(c.evidence.Value()); w.Str(c.detail);
  }
  // compatibility
  w.U32(static_cast<std::uint32_t>(r.compatibility.size()));
  for (const auto& c : r.compatibility) {
    w.U64(c.id.Value()); w.U64(c.generation.Value());
    w.U32(static_cast<std::uint32_t>(c.pins.size()));
    for (const auto& p : c.pins) { w.Str(p.name); w.Str(p.value); w.U64(p.generation.Value()); }
  }
}

bool ReadResource(Reader& r, Resource& out) {
  out.id = ResourceId(r.U64());
  out.generation = ResourceGeneration(r.U64());
  std::uint32_t cls = r.U32();
  if (!ValidEnum<ResourceClass>(cls)) return false;
  out.resource_class = static_cast<ResourceClass>(cls);
  out.node_id = NodeId(r.U64());
  out.provider = r.Str();
  out.authority_owner = r.Str();
  out.synthetic = r.U8() != 0;
  std::uint32_t h = r.U32(); if (!ValidEnum<ResourceHealth>(h)) return false; out.health = static_cast<ResourceHealth>(h);
  std::uint32_t rd = r.U32(); if (!ValidEnum<Readiness>(rd)) return false; out.readiness = static_cast<Readiness>(rd);
  std::uint32_t rc = r.U32(); if (!ValidEnum<Reachability>(rc)) return false; out.reachability = static_cast<Reachability>(rc);
  out.capacity.compute_units = r.Dbl(); out.capacity.accelerator_memory_bytes = r.Dbl();
  out.capacity.host_memory_bytes = r.Dbl(); out.capacity.expanded_memory_bytes = r.Dbl();
  out.capacity.storage_bytes = r.Dbl(); out.capacity.network_bandwidth_mbps = r.Dbl();
  out.capacity.network_latency_us = r.Dbl();
  std::uint32_t ex = r.U32(); if (ex > kMaxCount) return false;
  for (std::uint32_t i = 0; i < ex; ++i) { std::string k = r.Str(); double v = r.Dbl(); out.capacity.extra[k] = v; }
  std::uint32_t lc = r.U32(); if (lc > kMaxCount) return false;
  for (std::uint32_t i = 0; i < lc; ++i) out.locality.coords.push_back(r.Dbl());
  std::uint32_t lt = r.U32(); if (lt > kMaxCount) return false;
  for (std::uint32_t i = 0; i < lt; ++i) { std::string k = r.Str(); double v = r.Dbl(); out.locality.tags[k] = v; }
  out.failure_domain.id = FailureDomainId(r.U64());
  std::uint32_t fdt = r.U32(); if (!ValidEnum<FailureDomainType>(fdt)) return false; out.failure_domain.type = static_cast<FailureDomainType>(fdt);
  out.failure_domain.name = r.Str();
  out.failure_domain.synthetic = r.U8() != 0;
  out.evidence.id = EvidenceId(r.U64()); out.evidence.generation = EvidenceGeneration(r.U64());
  std::uint32_t es = r.U32(); if (!ValidEnum<EvidenceSource>(es)) return false; out.evidence.source = static_cast<EvidenceSource>(es);
  std::uint32_t est = r.U32(); if (!ValidEnum<EvidenceState>(est)) return false; out.evidence.state = static_cast<EvidenceState>(est);
  out.evidence.timestamp_ms = r.U64(); out.evidence.max_age_ms = r.U64();
  out.evidence.confidence = r.Dbl(); out.evidence.provenance = r.Str(); out.evidence.owner = r.Str();
  out.evidence.durable = r.U8() != 0;
  std::uint32_t caps = r.U32(); if (caps > kMaxCount) return false;
  for (std::uint32_t i = 0; i < caps; ++i) {
    Capability c;
    std::uint32_t ck = r.U32(); if (!ValidEnum<CapabilityKind>(ck)) return false; c.kind = static_cast<CapabilityKind>(ck);
    std::uint32_t cs = r.U32(); if (!ValidEnum<CapabilityState>(cs)) return false; c.state = static_cast<CapabilityState>(cs);
    c.generation = CapabilityGeneration(r.U64()); c.evidence = EvidenceId(r.U64()); c.detail = r.Str();
    out.capabilities.push_back(c);
  }
  std::uint32_t comps = r.U32(); if (comps > kMaxCount) return false;
  for (std::uint32_t i = 0; i < comps; ++i) {
    Compatibility c;
    c.id = CompatibilityId(r.U64()); c.generation = CompatibilityGeneration(r.U64());
    std::uint32_t pins = r.U32(); if (pins > kMaxCount) return false;
    for (std::uint32_t j = 0; j < pins; ++j) { CompatibilityPin p; p.name = r.Str(); p.value = r.Str(); p.generation = CompatibilityGeneration(r.U64()); c.pins.push_back(p); }
    out.compatibility.push_back(c);
  }
  return r.ok;
}

void WriteNode(Writer& w, const Node& n) {
  w.U64(n.id.Value()); w.U64(n.generation.Value());
  w.Str(n.hostname); w.Str(n.provider);
  w.U32(static_cast<std::uint32_t>(n.backend));
  w.U32(static_cast<std::uint32_t>(n.inventory.size()));
  for (const auto& rid : n.inventory) w.U64(rid.Value());
  w.U64(n.failure_domain.id.Value());
  w.U32(static_cast<std::uint32_t>(n.failure_domain.type));
  w.Str(n.failure_domain.name);
  w.U8(n.failure_domain.synthetic ? 1 : 0);
  w.U32(static_cast<std::uint32_t>(n.locality.coords.size()));
  for (double c : n.locality.coords) w.Dbl(c);
  w.U32(static_cast<std::uint32_t>(n.locality.tags.size()));
  for (const auto& kv : n.locality.tags) { w.Str(kv.first); w.Dbl(kv.second); }
  w.U32(static_cast<std::uint32_t>(n.reachability));
  w.U32(static_cast<std::uint32_t>(n.health));
  w.Str(n.worker_owner); w.U64(n.worker_boot.Value());
  w.U64(n.evidence_freshness_ms);
  w.U32(static_cast<std::uint32_t>(n.lifecycle));
  w.U8(n.synthetic ? 1 : 0);
}

bool ReadNode(Reader& r, Node& out) {
  out.id = NodeId(r.U64()); out.generation = NodeGeneration(r.U64());
  out.hostname = r.Str(); out.provider = r.Str();
  std::uint32_t bk = r.U32(); if (!ValidEnum<BackendKind>(bk)) return false; out.backend = static_cast<BackendKind>(bk);
  std::uint32_t inv = r.U32(); if (inv > kMaxCount) return false;
  for (std::uint32_t i = 0; i < inv; ++i) out.inventory.push_back(ResourceId(r.U64()));
  out.failure_domain.id = FailureDomainId(r.U64());
  std::uint32_t fdt = r.U32(); if (!ValidEnum<FailureDomainType>(fdt)) return false; out.failure_domain.type = static_cast<FailureDomainType>(fdt);
  out.failure_domain.name = r.Str();
  out.failure_domain.synthetic = r.U8() != 0;
  std::uint32_t lc = r.U32(); if (lc > kMaxCount) return false;
  for (std::uint32_t i = 0; i < lc; ++i) out.locality.coords.push_back(r.Dbl());
  std::uint32_t lt = r.U32(); if (lt > kMaxCount) return false;
  for (std::uint32_t i = 0; i < lt; ++i) { std::string k = r.Str(); double v = r.Dbl(); out.locality.tags[k] = v; }
  std::uint32_t rc = r.U32(); if (!ValidEnum<Reachability>(rc)) return false; out.reachability = static_cast<Reachability>(rc);
  std::uint32_t h = r.U32(); if (!ValidEnum<ResourceHealth>(h)) return false; out.health = static_cast<ResourceHealth>(h);
  out.worker_owner = r.Str(); out.worker_boot = WorkerBootId(r.U64());
  out.evidence_freshness_ms = r.U64();
  std::uint32_t lf = r.U32(); if (!ValidEnum<NodeLifecycle>(lf)) return false; out.lifecycle = static_cast<NodeLifecycle>(lf);
  out.synthetic = r.U8() != 0;
  return r.ok;
}

void WriteEvidence(Writer& w, const Evidence& e) {
  w.U64(e.id.Value()); w.U64(e.generation.Value());
  w.U32(static_cast<std::uint32_t>(e.source)); w.U32(static_cast<std::uint32_t>(e.state));
  w.U64(e.timestamp_ms); w.U64(e.max_age_ms); w.Dbl(e.confidence);
  w.Str(e.provenance); w.Str(e.owner); w.U8(e.durable ? 1 : 0);
}

bool ReadEvidence(Reader& r, Evidence& out) {
  out.id = EvidenceId(r.U64()); out.generation = EvidenceGeneration(r.U64());
  std::uint32_t s = r.U32(); if (!ValidEnum<EvidenceSource>(s)) return false; out.source = static_cast<EvidenceSource>(s);
  std::uint32_t st = r.U32(); if (!ValidEnum<EvidenceState>(st)) return false; out.state = static_cast<EvidenceState>(st);
  out.timestamp_ms = r.U64(); out.max_age_ms = r.U64(); out.confidence = r.Dbl();
  out.provenance = r.Str(); out.owner = r.Str(); out.durable = r.U8() != 0;
  return r.ok;
}

void WritePath(Writer& w, const PathEvidence& p) {
  w.U64(p.id.Value()); w.U64(p.generation.Value());
  w.U64(p.requirement.from.Value()); w.U64(p.requirement.to.Value());
  w.Dbl(p.requirement.min_bandwidth_mbps); w.Dbl(p.requirement.max_latency_us);
  w.U8(p.requirement.required ? 1 : 0); w.U8(p.requirement.direct ? 1 : 0);
  w.Str(p.requirement.protocol);
  w.U8(p.available ? 1 : 0); w.Dbl(p.bandwidth_mbps); w.Dbl(p.latency_us);
  w.U8(p.verified ? 1 : 0);
  w.Str(p.provider); w.Str(p.source);
  WriteEvidence(w, p.evidence);
}

bool ReadPath(Reader& r, PathEvidence& out) {
  out.id = PathEvidenceId(r.U64()); out.generation = PathGeneration(r.U64());
  out.requirement.from = NodeId(r.U64()); out.requirement.to = NodeId(r.U64());
  out.requirement.min_bandwidth_mbps = r.Dbl(); out.requirement.max_latency_us = r.Dbl();
  out.requirement.required = r.U8() != 0; out.requirement.direct = r.U8() != 0;
  out.requirement.protocol = r.Str();
  out.available = r.U8() != 0; out.bandwidth_mbps = r.Dbl(); out.latency_us = r.Dbl();
  out.verified = r.U8() != 0;
  out.provider = r.Str(); out.source = r.Str();
  return ReadEvidence(r, out.evidence);
}

void WriteGraph(Writer& w, const ResourceGraph& g) {
  w.U32(static_cast<std::uint32_t>(g.vertices.size()));
  for (const auto& id : g.vertices) w.U64(id.Value());
  w.U32(static_cast<std::uint32_t>(g.edges.size()));
  for (const auto& e : g.edges) {
    w.U64(e.from.Value()); w.U64(e.to.Value());
    w.U32(static_cast<std::uint32_t>(e.kind)); w.U8(e.required ? 1 : 0);
    w.Dbl(e.min_bandwidth_mbps); w.Dbl(e.max_latency_us);
    w.Str(e.protocol); w.Str(e.compat_facet);
  }
}

bool ReadGraph(Reader& r, ResourceGraph& out) {
  std::uint32_t nv = r.U32(); if (nv > kMaxCount) return false;
  for (std::uint32_t i = 0; i < nv; ++i) out.vertices.push_back(ResourceId(r.U64()));
  std::uint32_t ne = r.U32(); if (ne > kMaxCount) return false;
  for (std::uint32_t i = 0; i < ne; ++i) {
    GraphEdge e;
    e.from = ResourceId(r.U64()); e.to = ResourceId(r.U64());
    std::uint32_t k = r.U32(); if (!ValidEnum<GraphEdgeKind>(k)) return false; e.kind = static_cast<GraphEdgeKind>(k);
    e.required = r.U8() != 0; e.min_bandwidth_mbps = r.Dbl(); e.max_latency_us = r.Dbl();
    e.protocol = r.Str(); e.compat_facet = r.Str();
    out.edges.push_back(e);
  }
  return r.ok;
}

void WriteAuthority(Writer& w, const AuthorityTuple& a) {
  w.U64(a.coordinator_epoch.Value());
  w.U32(static_cast<std::uint32_t>(a.worker_boots.size()));
  for (const auto& v : a.worker_boots) w.U64(v.Value());
  w.U32(static_cast<std::uint32_t>(a.node_generations.size()));
  for (const auto& v : a.node_generations) w.U64(v.Value());
  w.U32(static_cast<std::uint32_t>(a.resource_generations.size()));
  for (const auto& v : a.resource_generations) w.U64(v.Value());
  w.U32(static_cast<std::uint32_t>(a.path_generations.size()));
  for (const auto& v : a.path_generations) w.U64(v.Value());
  w.U32(static_cast<std::uint32_t>(a.capability_generations.size()));
  for (const auto& v : a.capability_generations) w.U64(v.Value());
  w.U32(static_cast<std::uint32_t>(a.compatibility_generations.size()));
  for (const auto& v : a.compatibility_generations) w.U64(v.Value());
  w.U64(a.policy_generation.Value());
  w.U32(static_cast<std::uint32_t>(a.evidence_generations.size()));
  for (const auto& v : a.evidence_generations) w.U64(v.Value());
  w.U32(static_cast<std::uint32_t>(a.reservation_generations.size()));
  for (const auto& v : a.reservation_generations) w.U64(v.Value());
  w.U64(a.composition_generation.Value());
}

bool ReadAuthority(Reader& r, AuthorityTuple& out) {
  out.coordinator_epoch = CoordinatorEpoch(r.U64());
  std::uint32_t n = r.U32(); if (n > kMaxCount) return false; for (std::uint32_t i = 0; i < n; ++i) out.worker_boots.push_back(WorkerBootId(r.U64()));
  n = r.U32(); if (n > kMaxCount) return false; for (std::uint32_t i = 0; i < n; ++i) out.node_generations.push_back(NodeGeneration(r.U64()));
  n = r.U32(); if (n > kMaxCount) return false; for (std::uint32_t i = 0; i < n; ++i) out.resource_generations.push_back(ResourceGeneration(r.U64()));
  n = r.U32(); if (n > kMaxCount) return false; for (std::uint32_t i = 0; i < n; ++i) out.path_generations.push_back(PathGeneration(r.U64()));
  n = r.U32(); if (n > kMaxCount) return false; for (std::uint32_t i = 0; i < n; ++i) out.capability_generations.push_back(CapabilityGeneration(r.U64()));
  n = r.U32(); if (n > kMaxCount) return false; for (std::uint32_t i = 0; i < n; ++i) out.compatibility_generations.push_back(CompatibilityGeneration(r.U64()));
  out.policy_generation = PolicyGeneration(r.U64());
  n = r.U32(); if (n > kMaxCount) return false; for (std::uint32_t i = 0; i < n; ++i) out.evidence_generations.push_back(EvidenceGeneration(r.U64()));
  n = r.U32(); if (n > kMaxCount) return false; for (std::uint32_t i = 0; i < n; ++i) out.reservation_generations.push_back(ReservationGeneration(r.U64()));
  out.composition_generation = CompositionGeneration(r.U64());
  return r.ok;
}

void WriteRequest(Writer& w, const CompositionRequest& q) {
  w.U64(q.compute_units); w.U32(q.accelerator_count);
  w.U32(static_cast<std::uint32_t>(q.accelerator_architectures.size()));
  for (const auto& a : q.accelerator_architectures) w.Str(a);
  w.Dbl(q.accelerator_memory_bytes); w.Dbl(q.host_memory_bytes);
  w.Dbl(q.expanded_memory_bytes); w.Dbl(q.storage_bytes);
  w.U32(q.network_endpoint_count); w.Dbl(q.min_bandwidth_mbps); w.Dbl(q.max_latency_us);
  w.U32(static_cast<std::uint32_t>(q.mandatory_classes.size()));
  for (const auto& c : q.mandatory_classes) w.U32(static_cast<std::uint32_t>(c));
  w.U32(static_cast<std::uint32_t>(q.optional_classes.size()));
  for (const auto& c : q.optional_classes) w.U32(static_cast<std::uint32_t>(c));
  w.U8(q.require_direct_path ? 1 : 0); w.U8(q.require_coherence ? 1 : 0);
  w.U8(q.require_persistence ? 1 : 0); w.U8(q.require_isolation ? 1 : 0); w.U8(q.staging_allowance ? 1 : 0);
  w.U64(q.compatibility.Value());
  w.U64(q.policy.Value()); w.U64(q.policy_generation.Value());
  w.U64(q.consumer.Value()); w.U64(q.consumer_generation.Value());
}

bool ReadRequest(Reader& r, CompositionRequest& out) {
  out.compute_units = r.U64(); out.accelerator_count = r.U32();
  std::uint32_t na = r.U32(); if (na > kMaxCount) return false; for (std::uint32_t i = 0; i < na; ++i) out.accelerator_architectures.push_back(r.Str());
  out.accelerator_memory_bytes = r.Dbl(); out.host_memory_bytes = r.Dbl();
  out.expanded_memory_bytes = r.Dbl(); out.storage_bytes = r.Dbl();
  out.network_endpoint_count = r.U32(); out.min_bandwidth_mbps = r.Dbl(); out.max_latency_us = r.Dbl();
  std::uint32_t nm = r.U32(); if (nm > kMaxCount) return false; for (std::uint32_t i = 0; i < nm; ++i) { std::uint32_t c = r.U32(); if (!ValidEnum<ResourceClass>(c)) return false; out.mandatory_classes.insert(static_cast<ResourceClass>(c)); }
  std::uint32_t no = r.U32(); if (no > kMaxCount) return false; for (std::uint32_t i = 0; i < no; ++i) { std::uint32_t c = r.U32(); if (!ValidEnum<ResourceClass>(c)) return false; out.optional_classes.insert(static_cast<ResourceClass>(c)); }
  out.require_direct_path = r.U8() != 0; out.require_coherence = r.U8() != 0;
  out.require_persistence = r.U8() != 0; out.require_isolation = r.U8() != 0; out.staging_allowance = r.U8() != 0;
  out.compatibility = CompatibilityId(r.U64());
  out.policy = PolicyId(r.U64()); out.policy_generation = PolicyGeneration(r.U64());
  out.consumer = ConsumerId(r.U64()); out.consumer_generation = ConsumerGeneration(r.U64());
  return r.ok;
}

void WriteComposition(Writer& w, const Composition& c) {
  w.U64(c.id.Value()); w.U64(c.generation.Value());
  w.U64(c.resource_set_id.Value()); w.U64(c.resource_set_generation.Value());
  w.U32(static_cast<std::uint32_t>(c.nodes.size()));
  for (const auto& n : c.nodes) w.U64(n.Value());
  WriteRequest(w, c.request);
  WriteGraph(w, c.graph);
  WriteAuthority(w, c.authority);
  w.U32(static_cast<std::uint32_t>(c.lifecycle));
  w.U64(c.reservation.Value());
  w.U32(static_cast<std::uint32_t>(c.outcome));
  w.Dbl(c.rank.total);
  w.U32(static_cast<std::uint32_t>(c.paths.size()));
  for (const auto& p : c.paths) WritePath(w, p);
}

bool ReadComposition(Reader& r, Composition& out) {
  out.id = CompositionId(r.U64()); out.generation = CompositionGeneration(r.U64());
  out.resource_set_id = ResourceSetId(r.U64()); out.resource_set_generation = ResourceSetGeneration(r.U64());
  std::uint32_t nn = r.U32(); if (nn > kMaxCount) return false; for (std::uint32_t i = 0; i < nn; ++i) out.nodes.push_back(NodeId(r.U64()));
  if (!ReadRequest(r, out.request)) return false;
  if (!ReadGraph(r, out.graph)) return false;
  if (!ReadAuthority(r, out.authority)) return false;
  std::uint32_t lc = r.U32(); if (!ValidEnum<CompositionLifecycle>(lc)) return false; out.lifecycle = static_cast<CompositionLifecycle>(lc);
  out.reservation = ReservationId(r.U64());
  std::uint32_t oc = r.U32(); if (!ValidEnum<CompositionOutcome>(oc)) return false; out.outcome = static_cast<CompositionOutcome>(oc);
  out.rank.total = r.Dbl();
  std::uint32_t np = r.U32(); if (np > kMaxCount) return false;
  for (std::uint32_t i = 0; i < np; ++i) { PathEvidence p; if (!ReadPath(r, p)) return false; out.paths.push_back(p); }
  return r.ok;
}

bool SaveImpl(const DurableState& s, const std::string& path, std::string& err) {
  try {
    Writer w;
    w.U32(kMagic);
    w.U32(kFormatVersion);
    w.U64(s.epoch.Value());
    w.U32(static_cast<std::uint32_t>(s.nodes.size()));
    for (const auto& n : s.nodes) WriteNode(w, n);
    w.U32(static_cast<std::uint32_t>(s.resources.size()));
    for (const auto& r : s.resources) WriteResource(w, r);
    w.U32(static_cast<std::uint32_t>(s.policies.size()));
    for (const auto& kv : s.policies) { w.U64(kv.first.Value()); w.U64(kv.second.Value()); }
    w.U32(static_cast<std::uint32_t>(s.compositions.size()));
    for (const auto& c : s.compositions) WriteComposition(w, c);
    w.U64(Checksum(w.buf.data(), w.buf.size()));

    // Atomic replacement: write to a temp file, then move.
    std::string tmp = path + ".tmp";
    std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
    if (!ofs) { err = "cannot open temp path for write"; return false; }
    ofs.write(reinterpret_cast<const char*>(w.buf.data()), static_cast<std::streamsize>(w.buf.size()));
    ofs.close();
    if (!ofs.good()) { err = "write failed"; return false; }
#ifdef _WIN32
    std::remove(path.c_str());
#endif
    std::ifstream ifs(tmp, std::ios::binary);
    std::ofstream ofs2(path, std::ios::binary | std::ios::trunc);
    if (!ofs2) { err = "cannot open final path"; return false; }
    ifs.seekg(0, std::ios::end); std::streamsize sz = ifs.tellg(); ifs.seekg(0);
    std::vector<char> b(static_cast<std::size_t>(sz));
    ifs.read(b.data(), sz);
    ofs2.write(b.data(), static_cast<std::streamsize>(b.size()));
    ofs2.close();
#ifdef _WIN32
    std::remove(tmp.c_str());
#endif
    return true;
  } catch (const std::exception& e) { err = e.what(); return false; }
}

bool LoadImpl(const std::string& path, DurableState& out, std::string& err) {
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) { err = "cannot open path for read"; return false; }
  ifs.seekg(0, std::ios::end); std::streamsize sz = ifs.tellg(); ifs.seekg(0);
  if (sz <= 0) { err = "empty/truncated file"; return false; }
  std::vector<std::uint8_t> buf(static_cast<std::size_t>(sz));
  ifs.read(reinterpret_cast<char*>(buf.data()), sz);
  if (!ifs) { err = "read failed (truncation)"; return false; }
  Reader r(buf);
  if (r.U32() != kMagic) { err = "bad magic"; return false; }
  if (r.U32() != kFormatVersion) { err = "unsupported format version"; return false; }
  // Verify checksum over everything except the trailing checksum word.
  if (buf.size() < 8) { err = "truncated (no checksum)"; return false; }
  std::uint64_t stored = 0;
  { Reader cr(buf); cr.pos = buf.size() - 8; stored = cr.U64(); }
  std::uint64_t computed = Checksum(buf.data(), buf.size() - 8);
  if (stored != computed) { err = "checksum mismatch"; return false; }
  // r.pos is already at 8 (after magic+version); continue reading the epoch.
  out.epoch = CoordinatorEpoch(r.U64());
  std::uint32_t nn = r.U32(); if (nn > kMaxCount) { err = "node count overflow"; return false; }
  for (std::uint32_t i = 0; i < nn; ++i) { Node n; if (!ReadNode(r, n)) { err = "malformed node"; return false; } out.nodes.push_back(n); }
  std::uint32_t nr = r.U32(); if (nr > kMaxCount) { err = "resource count overflow"; return false; }
  for (std::uint32_t i = 0; i < nr; ++i) { Resource res; if (!ReadResource(r, res)) { err = "malformed resource"; return false; } out.resources.push_back(res); }
  std::uint32_t np = r.U32(); if (np > kMaxCount) { err = "policy count overflow"; return false; }
  for (std::uint32_t i = 0; i < np; ++i) { PolicyId p(r.U64()); PolicyGeneration g(r.U64()); out.policies[p] = g; }
  std::uint32_t nc = r.U32(); if (nc > kMaxCount) { err = "composition count overflow"; return false; }
  for (std::uint32_t i = 0; i < nc; ++i) { Composition c; if (!ReadComposition(r, c)) { err = "malformed composition"; return false; } out.compositions.push_back(c); }
  if (!r.ok) { err = "truncated payload"; return false; }
  return true;
}

}  // namespace

bool Save(const DurableState& state, const std::string& path, std::string& err) { return SaveImpl(state, path, err); }
bool Load(const std::string& path, DurableState& out, std::string& err) { return LoadImpl(path, out, err); }

}  // namespace persist
}  // namespace dfabric