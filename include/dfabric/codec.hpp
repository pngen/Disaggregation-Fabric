// Disaggregation Fabric — shared wire codec (header-only).
// Binary serialization for node/resource/composition types used across the
// coordinator <-> worker and coordinator <-> control-client protocol, and by
// persistence. Bounds-checked with canonical enum validation on decode.
#pragma once
#include "dfabric/ids.hpp"
#include "dfabric/enums.hpp"
#include "dfabric/model.hpp"
#include "dfabric/graph.hpp"
#include "dfabric/requirement.hpp"
#include "dfabric/composition.hpp"
#include "dfabric/authority.hpp"
#include <vector>
#include <string>
#include <map>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <type_traits>

namespace dfabric {

// Max string length accepted by the wire codec (bounded).
inline constexpr std::uint32_t kWireMaxLen = 1u << 15;
inline constexpr std::uint32_t kWireMaxCount = 1u << 20;

class WireWriter {
 public:
  void U8(std::uint8_t v) { buf_.push_back(v); }
  void U16(std::uint16_t v) { buf_.push_back(v & 0xff); buf_.push_back((v >> 8) & 0xff); }
  void U32(std::uint32_t v) { for (int i = 0; i < 4; ++i) buf_.push_back((v >> (8 * i)) & 0xff); }
  void U64(std::uint64_t v) { for (int i = 0; i < 8; ++i) buf_.push_back((v >> (8 * i)) & 0xff); }
  void Dbl(double d) { std::uint64_t u; std::memcpy(&u, &d, sizeof(u)); U64(u); }
  void Str(const std::string& s) {
    if (s.size() > kWireMaxLen) throw std::runtime_error("wire: string too long");
    U32(static_cast<std::uint32_t>(s.size()));
    buf_.insert(buf_.end(), s.begin(), s.end());
  }
  void Bytes(const void* p, std::size_t n) { const auto* b = static_cast<const std::uint8_t*>(p); buf_.insert(buf_.end(), b, b + n); }
  const std::vector<std::uint8_t>& buf() const { return buf_; }
  std::vector<std::uint8_t> Take() { return std::move(buf_); }
 private:
  std::vector<std::uint8_t> buf_;
};

class WireReader {
 public:
  WireReader(const std::uint8_t* d, std::size_t n) : data_(d), size_(n) {}
  bool ok() const { return ok_; }
  std::size_t pos() const { return pos_; }

  bool Need(std::size_t n) { return ok_ && n <= (size_ - pos_) && pos_ + n >= pos_; }
  std::uint8_t U8() { if (!Need(1)) { ok_ = false; return 0; } return data_[pos_++]; }
  std::uint16_t U16() { if (!Need(2)) { ok_ = false; return 0; } std::uint16_t v = static_cast<std::uint16_t>(data_[pos_]) | (static_cast<std::uint16_t>(data_[pos_ + 1]) << 8); pos_ += 2; return v; }
  std::uint32_t U32() { if (!Need(4)) { ok_ = false; return 0; } std::uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(data_[pos_ + i]) << (8 * i); pos_ += 4; return v; }
  std::uint64_t U64() { if (!Need(8)) { ok_ = false; return 0; } std::uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(data_[pos_ + i]) << (8 * i); pos_ += 8; return v; }
  double Dbl() { std::uint64_t u = U64(); double d; std::memcpy(&d, &u, sizeof(d)); return d; }
  std::string Str() { std::uint32_t n = U32(); if (!ok_ || n > kWireMaxLen || !Need(n)) { ok_ = false; return {}; } std::string s(reinterpret_cast<const char*>(data_ + pos_), n); pos_ += n; return s; }
  void Skip(std::size_t n) { if (!Need(n)) { ok_ = false; return; } pos_ += n; }

 private:
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t pos_ = 0;
  bool ok_ = true;
};

inline WireReader MakeReader(const std::vector<std::uint8_t>& v) { return WireReader(v.data(), v.size()); }

// ---- shared enum validation (rejects values outside a canonical table) -----
template <typename E>
inline bool WireValidEnum(std::uint32_t v) {
  return v >= static_cast<std::uint32_t>(EnumTraits<E>::Min()) && v <= EnumTraits<E>::MaxValue;
}

// ---- encode/decode helpers for core types ----------------------------------
inline void EncodeEvidence(WireWriter& w, const Evidence& e) {
  w.U64(e.id.Value()); w.U64(e.generation.Value());
  w.U32(static_cast<std::uint32_t>(e.source)); w.U32(static_cast<std::uint32_t>(e.state));
  w.U64(e.timestamp_ms); w.U64(e.max_age_ms); w.Dbl(e.confidence);
  w.Str(e.provenance); w.Str(e.owner); w.U8(e.durable ? 1 : 0);
}
inline bool DecodeEvidence(WireReader& r, Evidence& e) {
  e.id = EvidenceId(r.U64()); e.generation = EvidenceGeneration(r.U64());
  std::uint32_t s = r.U32(); if (!WireValidEnum<EvidenceSource>(s)) return false; e.source = static_cast<EvidenceSource>(s);
  std::uint32_t st = r.U32(); if (!WireValidEnum<EvidenceState>(st)) return false; e.state = static_cast<EvidenceState>(st);
  e.timestamp_ms = r.U64(); e.max_age_ms = r.U64(); e.confidence = r.Dbl();
  e.provenance = r.Str(); e.owner = r.Str(); e.durable = r.U8() != 0;
  return r.ok();
}

inline void EncodeCapability(WireWriter& w, const Capability& c) {
  w.U32(static_cast<std::uint32_t>(c.kind)); w.U32(static_cast<std::uint32_t>(c.state));
  w.U64(c.generation.Value()); w.U64(c.evidence.Value()); w.Str(c.detail);
}
inline bool DecodeCapability(WireReader& r, Capability& c) {
  std::uint32_t k = r.U32(); if (!WireValidEnum<CapabilityKind>(k)) return false; c.kind = static_cast<CapabilityKind>(k);
  std::uint32_t s = r.U32(); if (!WireValidEnum<CapabilityState>(s)) return false; c.state = static_cast<CapabilityState>(s);
  c.generation = CapabilityGeneration(r.U64()); c.evidence = EvidenceId(r.U64()); c.detail = r.Str();
  return r.ok();
}

inline void EncodeCompatibility(WireWriter& w, const Compatibility& c) {
  w.U64(c.id.Value()); w.U64(c.generation.Value());
  w.U32(static_cast<std::uint32_t>(c.pins.size()));
  for (const auto& p : c.pins) { w.Str(p.name); w.Str(p.value); w.U64(p.generation.Value()); }
}
inline bool DecodeCompatibility(WireReader& r, Compatibility& c) {
  c.id = CompatibilityId(r.U64()); c.generation = CompatibilityGeneration(r.U64());
  std::uint32_t n = r.U32(); if (n > kWireMaxCount) return false;
  for (std::uint32_t i = 0; i < n; ++i) { CompatibilityPin p; p.name = r.Str(); p.value = r.Str(); p.generation = CompatibilityGeneration(r.U64()); c.pins.push_back(p); }
  return r.ok();
}

inline void EncodeResource(WireWriter& w, const Resource& res) {
  w.U64(res.id.Value()); w.U64(res.generation.Value());
  w.U32(static_cast<std::uint32_t>(res.resource_class));
  w.U64(res.node_id.Value());
  w.Str(res.provider); w.Str(res.authority_owner); w.U8(res.synthetic ? 1 : 0);
  w.U32(static_cast<std::uint32_t>(res.health));
  w.U32(static_cast<std::uint32_t>(res.readiness));
  w.U32(static_cast<std::uint32_t>(res.reachability));
  w.Dbl(res.capacity.compute_units); w.Dbl(res.capacity.accelerator_memory_bytes);
  w.Dbl(res.capacity.host_memory_bytes); w.Dbl(res.capacity.expanded_memory_bytes);
  w.Dbl(res.capacity.storage_bytes); w.Dbl(res.capacity.network_bandwidth_mbps);
  w.Dbl(res.capacity.network_latency_us);
  w.U32(static_cast<std::uint32_t>(res.capacity.extra.size()));
  for (const auto& kv : res.capacity.extra) { w.Str(kv.first); w.Dbl(kv.second); }
  w.U32(static_cast<std::uint32_t>(res.locality.coords.size()));
  for (double c : res.locality.coords) w.Dbl(c);
  w.U32(static_cast<std::uint32_t>(res.locality.tags.size()));
  for (const auto& kv : res.locality.tags) { w.Str(kv.first); w.Dbl(kv.second); }
  w.U64(res.failure_domain.id.Value()); w.U32(static_cast<std::uint32_t>(res.failure_domain.type));
  w.Str(res.failure_domain.name); w.U8(res.failure_domain.synthetic ? 1 : 0);
  EncodeEvidence(w, res.evidence);
  w.U32(static_cast<std::uint32_t>(res.capabilities.size()));
  for (const auto& c : res.capabilities) EncodeCapability(w, c);
  w.U32(static_cast<std::uint32_t>(res.compatibility.size()));
  for (const auto& c : res.compatibility) EncodeCompatibility(w, c);
}
inline bool DecodeResource(WireReader& r, Resource& res) {
  res.id = ResourceId(r.U64()); res.generation = ResourceGeneration(r.U64());
  std::uint32_t cls = r.U32(); if (!WireValidEnum<ResourceClass>(cls)) return false; res.resource_class = static_cast<ResourceClass>(cls);
  res.node_id = NodeId(r.U64());
  res.provider = r.Str(); res.authority_owner = r.Str(); res.synthetic = r.U8() != 0;
  std::uint32_t h = r.U32(); if (!WireValidEnum<ResourceHealth>(h)) return false; res.health = static_cast<ResourceHealth>(h);
  std::uint32_t rd = r.U32(); if (!WireValidEnum<Readiness>(rd)) return false; res.readiness = static_cast<Readiness>(rd);
  std::uint32_t rc = r.U32(); if (!WireValidEnum<Reachability>(rc)) return false; res.reachability = static_cast<Reachability>(rc);
  res.capacity.compute_units = r.Dbl(); res.capacity.accelerator_memory_bytes = r.Dbl();
  res.capacity.host_memory_bytes = r.Dbl(); res.capacity.expanded_memory_bytes = r.Dbl();
  res.capacity.storage_bytes = r.Dbl(); res.capacity.network_bandwidth_mbps = r.Dbl();
  res.capacity.network_latency_us = r.Dbl();
  std::uint32_t ex = r.U32(); if (ex > kWireMaxCount) return false;
  for (std::uint32_t i = 0; i < ex; ++i) { std::string k = r.Str(); double v = r.Dbl(); res.capacity.extra[k] = v; }
  std::uint32_t lc = r.U32(); if (lc > kWireMaxCount) return false;
  for (std::uint32_t i = 0; i < lc; ++i) res.locality.coords.push_back(r.Dbl());
  std::uint32_t lt = r.U32(); if (lt > kWireMaxCount) return false;
  for (std::uint32_t i = 0; i < lt; ++i) { std::string k = r.Str(); double v = r.Dbl(); res.locality.tags[k] = v; }
  res.failure_domain.id = FailureDomainId(r.U64());
  std::uint32_t fdt = r.U32(); if (!WireValidEnum<FailureDomainType>(fdt)) return false; res.failure_domain.type = static_cast<FailureDomainType>(fdt);
  res.failure_domain.name = r.Str(); res.failure_domain.synthetic = r.U8() != 0;
  if (!DecodeEvidence(r, res.evidence)) return false;
  std::uint32_t caps = r.U32(); if (caps > kWireMaxCount) return false;
  for (std::uint32_t i = 0; i < caps; ++i) { Capability c; if (!DecodeCapability(r, c)) return false; res.capabilities.push_back(c); }
  std::uint32_t comps = r.U32(); if (comps > kWireMaxCount) return false;
  for (std::uint32_t i = 0; i < comps; ++i) { Compatibility c; if (!DecodeCompatibility(r, c)) return false; res.compatibility.push_back(c); }
  return r.ok();
}

inline void EncodeNode(WireWriter& w, const Node& n) {
  w.U64(n.id.Value()); w.U64(n.generation.Value());
  w.Str(n.hostname); w.Str(n.provider);
  w.U32(static_cast<std::uint32_t>(n.backend));
  w.U32(static_cast<std::uint32_t>(n.inventory.size()));
  for (const auto& rid : n.inventory) w.U64(rid.Value());
  w.U64(n.failure_domain.id.Value()); w.U32(static_cast<std::uint32_t>(n.failure_domain.type));
  w.Str(n.failure_domain.name); w.U8(n.failure_domain.synthetic ? 1 : 0);
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
inline bool DecodeNode(WireReader& r, Node& n) {
  n.id = NodeId(r.U64()); n.generation = NodeGeneration(r.U64());
  n.hostname = r.Str(); n.provider = r.Str();
  std::uint32_t bk = r.U32(); if (!WireValidEnum<BackendKind>(bk)) return false; n.backend = static_cast<BackendKind>(bk);
  std::uint32_t inv = r.U32(); if (inv > kWireMaxCount) return false;
  for (std::uint32_t i = 0; i < inv; ++i) n.inventory.push_back(ResourceId(r.U64()));
  n.failure_domain.id = FailureDomainId(r.U64());
  std::uint32_t fdt = r.U32(); if (!WireValidEnum<FailureDomainType>(fdt)) return false; n.failure_domain.type = static_cast<FailureDomainType>(fdt);
  n.failure_domain.name = r.Str(); n.failure_domain.synthetic = r.U8() != 0;
  std::uint32_t lc = r.U32(); if (lc > kWireMaxCount) return false;
  for (std::uint32_t i = 0; i < lc; ++i) n.locality.coords.push_back(r.Dbl());
  std::uint32_t lt = r.U32(); if (lt > kWireMaxCount) return false;
  for (std::uint32_t i = 0; i < lt; ++i) { std::string k = r.Str(); double v = r.Dbl(); n.locality.tags[k] = v; }
  std::uint32_t rc = r.U32(); if (!WireValidEnum<Reachability>(rc)) return false; n.reachability = static_cast<Reachability>(rc);
  std::uint32_t h = r.U32(); if (!WireValidEnum<ResourceHealth>(h)) return false; n.health = static_cast<ResourceHealth>(h);
  n.worker_owner = r.Str(); n.worker_boot = WorkerBootId(r.U64());
  n.evidence_freshness_ms = r.U64();
  std::uint32_t lf = r.U32(); if (!WireValidEnum<NodeLifecycle>(lf)) return false; n.lifecycle = static_cast<NodeLifecycle>(lf);
  n.synthetic = r.U8() != 0;
  return r.ok();
}

inline void EncodePath(WireWriter& w, const PathEvidence& p) {
  w.U64(p.id.Value()); w.U64(p.generation.Value());
  w.U64(p.requirement.from.Value()); w.U64(p.requirement.to.Value());
  w.Dbl(p.requirement.min_bandwidth_mbps); w.Dbl(p.requirement.max_latency_us);
  w.U8(p.requirement.required ? 1 : 0); w.U8(p.requirement.direct ? 1 : 0);
  w.Str(p.requirement.protocol);
  w.U8(p.available ? 1 : 0); w.Dbl(p.bandwidth_mbps); w.Dbl(p.latency_us); w.U8(p.verified ? 1 : 0);
  w.Str(p.provider); w.Str(p.source);
  EncodeEvidence(w, p.evidence);
}
inline bool DecodePath(WireReader& r, PathEvidence& p) {
  p.id = PathEvidenceId(r.U64()); p.generation = PathGeneration(r.U64());
  p.requirement.from = NodeId(r.U64()); p.requirement.to = NodeId(r.U64());
  p.requirement.min_bandwidth_mbps = r.Dbl(); p.requirement.max_latency_us = r.Dbl();
  p.requirement.required = r.U8() != 0; p.requirement.direct = r.U8() != 0; p.requirement.protocol = r.Str();
  p.available = r.U8() != 0; p.bandwidth_mbps = r.Dbl(); p.latency_us = r.Dbl(); p.verified = r.U8() != 0;
  p.provider = r.Str(); p.source = r.Str();
  return DecodeEvidence(r, p.evidence);
}

inline void EncodeGraph(WireWriter& w, const ResourceGraph& g) {
  w.U32(static_cast<std::uint32_t>(g.vertices.size()));
  for (const auto& id : g.vertices) w.U64(id.Value());
  w.U32(static_cast<std::uint32_t>(g.edges.size()));
  for (const auto& e : g.edges) {
    w.U64(e.from.Value()); w.U64(e.to.Value()); w.U32(static_cast<std::uint32_t>(e.kind));
    w.U8(e.required ? 1 : 0); w.Dbl(e.min_bandwidth_mbps); w.Dbl(e.max_latency_us);
    w.Str(e.protocol); w.Str(e.compat_facet);
  }
}
inline bool DecodeGraph(WireReader& r, ResourceGraph& g) {
  std::uint32_t nv = r.U32(); if (nv > kWireMaxCount) return false;
  for (std::uint32_t i = 0; i < nv; ++i) g.vertices.push_back(ResourceId(r.U64()));
  std::uint32_t ne = r.U32(); if (ne > kWireMaxCount) return false;
  for (std::uint32_t i = 0; i < ne; ++i) {
    GraphEdge e; e.from = ResourceId(r.U64()); e.to = ResourceId(r.U64());
    std::uint32_t k = r.U32(); if (!WireValidEnum<GraphEdgeKind>(k)) return false; e.kind = static_cast<GraphEdgeKind>(k);
    e.required = r.U8() != 0; e.min_bandwidth_mbps = r.Dbl(); e.max_latency_us = r.Dbl();
    e.protocol = r.Str(); e.compat_facet = r.Str(); g.edges.push_back(e);
  }
  return r.ok();
}

inline void EncodeAuthority(WireWriter& w, const AuthorityTuple& a) {
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
inline bool DecodeAuthority(WireReader& r, AuthorityTuple& a) {
  a.coordinator_epoch = CoordinatorEpoch(r.U64());
  std::uint32_t n = r.U32(); if (n > kWireMaxCount) return false; for (std::uint32_t i = 0; i < n; ++i) a.worker_boots.push_back(WorkerBootId(r.U64()));
  n = r.U32(); if (n > kWireMaxCount) return false; for (std::uint32_t i = 0; i < n; ++i) a.node_generations.push_back(NodeGeneration(r.U64()));
  n = r.U32(); if (n > kWireMaxCount) return false; for (std::uint32_t i = 0; i < n; ++i) a.resource_generations.push_back(ResourceGeneration(r.U64()));
  n = r.U32(); if (n > kWireMaxCount) return false; for (std::uint32_t i = 0; i < n; ++i) a.path_generations.push_back(PathGeneration(r.U64()));
  n = r.U32(); if (n > kWireMaxCount) return false; for (std::uint32_t i = 0; i < n; ++i) a.capability_generations.push_back(CapabilityGeneration(r.U64()));
  n = r.U32(); if (n > kWireMaxCount) return false; for (std::uint32_t i = 0; i < n; ++i) a.compatibility_generations.push_back(CompatibilityGeneration(r.U64()));
  a.policy_generation = PolicyGeneration(r.U64());
  n = r.U32(); if (n > kWireMaxCount) return false; for (std::uint32_t i = 0; i < n; ++i) a.evidence_generations.push_back(EvidenceGeneration(r.U64()));
  n = r.U32(); if (n > kWireMaxCount) return false; for (std::uint32_t i = 0; i < n; ++i) a.reservation_generations.push_back(ReservationGeneration(r.U64()));
  a.composition_generation = CompositionGeneration(r.U64());
  return r.ok();
}

inline void EncodeRequest(WireWriter& w, const CompositionRequest& q) {
  w.U64(q.compute_units); w.U32(q.accelerator_count);
  w.U32(static_cast<std::uint32_t>(q.accelerator_architectures.size()));
  for (const auto& a : q.accelerator_architectures) w.Str(a);
  w.Dbl(q.accelerator_memory_bytes); w.Dbl(q.host_memory_bytes); w.Dbl(q.expanded_memory_bytes);
  w.Dbl(q.storage_bytes); w.U32(q.network_endpoint_count); w.Dbl(q.min_bandwidth_mbps); w.Dbl(q.max_latency_us);
  w.U32(static_cast<std::uint32_t>(q.mandatory_classes.size()));
  for (const auto& c : q.mandatory_classes) w.U32(static_cast<std::uint32_t>(c));
  w.U32(static_cast<std::uint32_t>(q.optional_classes.size()));
  for (const auto& c : q.optional_classes) w.U32(static_cast<std::uint32_t>(c));
  w.U8(q.require_direct_path ? 1 : 0); w.U8(q.require_coherence ? 1 : 0); w.U8(q.require_persistence ? 1 : 0);
  w.U8(q.require_isolation ? 1 : 0); w.U8(q.staging_allowance ? 1 : 0); w.U8(q.ignore_reachability ? 1 : 0);
  w.U64(q.compatibility.Value()); w.U64(q.policy.Value()); w.U64(q.policy_generation.Value());
  w.U64(q.consumer.Value()); w.U64(q.consumer_generation.Value());
}
inline bool DecodeRequest(WireReader& r, CompositionRequest& q) {
  q.compute_units = r.U64(); q.accelerator_count = r.U32();
  std::uint32_t na = r.U32(); if (na > kWireMaxCount) return false;
  for (std::uint32_t i = 0; i < na; ++i) q.accelerator_architectures.push_back(r.Str());
  q.accelerator_memory_bytes = r.Dbl(); q.host_memory_bytes = r.Dbl(); q.expanded_memory_bytes = r.Dbl();
  q.storage_bytes = r.Dbl(); q.network_endpoint_count = r.U32(); q.min_bandwidth_mbps = r.Dbl(); q.max_latency_us = r.Dbl();
  std::uint32_t nm = r.U32(); if (nm > kWireMaxCount) return false;
  for (std::uint32_t i = 0; i < nm; ++i) { std::uint32_t c = r.U32(); if (!WireValidEnum<ResourceClass>(c)) return false; q.mandatory_classes.insert(static_cast<ResourceClass>(c)); }
  std::uint32_t no = r.U32(); if (no > kWireMaxCount) return false;
  for (std::uint32_t i = 0; i < no; ++i) { std::uint32_t c = r.U32(); if (!WireValidEnum<ResourceClass>(c)) return false; q.optional_classes.insert(static_cast<ResourceClass>(c)); }
  q.require_direct_path = r.U8() != 0; q.require_coherence = r.U8() != 0; q.require_persistence = r.U8() != 0;
  q.require_isolation = r.U8() != 0; q.staging_allowance = r.U8() != 0; q.ignore_reachability = r.U8() != 0;
  q.compatibility = CompatibilityId(r.U64()); q.policy = PolicyId(r.U64()); q.policy_generation = PolicyGeneration(r.U64());
  q.consumer = ConsumerId(r.U64()); q.consumer_generation = ConsumerGeneration(r.U64());
  return r.ok();
}

inline void EncodePlan(WireWriter& w, const CompositionPlan& p) {
  w.U64(p.id.Value()); w.U64(p.generation.Value());
  EncodeGraph(w, p.graph);
  w.U32(static_cast<std::uint32_t>(p.nodes.size()));
  for (const auto& n : p.nodes) w.U64(n.Value());
  EncodeRequest(w, p.request);
  w.Dbl(p.rank.total);
  w.U32(static_cast<std::uint32_t>(p.rank.factors.size()));
  for (const auto& f : p.rank.factors) { w.Str(f.name); w.Dbl(f.weight); w.Dbl(f.value); w.Str(f.detail); }
  EncodeAuthority(w, p.authority);
  w.U32(static_cast<std::uint32_t>(p.lifecycle));
  w.U32(static_cast<std::uint32_t>(p.paths.size()));
  for (const auto& x : p.paths) EncodePath(w, x);
}
inline bool DecodePlan(WireReader& r, CompositionPlan& p) {
  p.id = CompositionId(r.U64()); p.generation = CompositionGeneration(r.U64());
  if (!DecodeGraph(r, p.graph)) return false;
  std::uint32_t nn = r.U32(); if (nn > kWireMaxCount) return false;
  for (std::uint32_t i = 0; i < nn; ++i) p.nodes.push_back(NodeId(r.U64()));
  if (!DecodeRequest(r, p.request)) return false;
  p.rank.total = r.Dbl();
  std::uint32_t nf = r.U32(); if (nf > kWireMaxCount) return false;
  for (std::uint32_t i = 0; i < nf; ++i) { RankingFactor f; f.name = r.Str(); f.weight = r.Dbl(); f.value = r.Dbl(); f.detail = r.Str(); p.rank.factors.push_back(f); }
  if (!DecodeAuthority(r, p.authority)) return false;
  std::uint32_t lc = r.U32(); if (!WireValidEnum<CompositionLifecycle>(lc)) return false; p.lifecycle = static_cast<CompositionLifecycle>(lc);
  std::uint32_t np = r.U32(); if (np > kWireMaxCount) return false;
  for (std::uint32_t i = 0; i < np; ++i) { PathEvidence x; if (!DecodePath(r, x)) return false; p.paths.push_back(x); }
  return r.ok();
}

inline void EncodeComposition(WireWriter& w, const Composition& c) {
  w.U64(c.id.Value()); w.U64(c.generation.Value());
  w.U64(c.resource_set_id.Value()); w.U64(c.resource_set_generation.Value());
  w.U32(static_cast<std::uint32_t>(c.nodes.size()));
  for (const auto& n : c.nodes) w.U64(n.Value());
  EncodeRequest(w, c.request);
  EncodeGraph(w, c.graph);
  EncodeAuthority(w, c.authority);
  w.U32(static_cast<std::uint32_t>(c.lifecycle));
  w.U64(c.reservation.Value());
  w.U32(static_cast<std::uint32_t>(c.outcome));
  w.Dbl(c.rank.total);
  w.U32(static_cast<std::uint32_t>(c.paths.size()));
  for (const auto& x : c.paths) EncodePath(w, x);
  w.Str(c.explanation);
}
inline bool DecodeComposition(WireReader& r, Composition& c) {
  c.id = CompositionId(r.U64()); c.generation = CompositionGeneration(r.U64());
  c.resource_set_id = ResourceSetId(r.U64()); c.resource_set_generation = ResourceSetGeneration(r.U64());
  std::uint32_t nn = r.U32(); if (nn > kWireMaxCount) return false;
  for (std::uint32_t i = 0; i < nn; ++i) c.nodes.push_back(NodeId(r.U64()));
  if (!DecodeRequest(r, c.request)) return false;
  if (!DecodeGraph(r, c.graph)) return false;
  if (!DecodeAuthority(r, c.authority)) return false;
  std::uint32_t lc = r.U32(); if (!WireValidEnum<CompositionLifecycle>(lc)) return false; c.lifecycle = static_cast<CompositionLifecycle>(lc);
  c.reservation = ReservationId(r.U64());
  std::uint32_t oc = r.U32(); if (!WireValidEnum<CompositionOutcome>(oc)) return false; c.outcome = static_cast<CompositionOutcome>(oc);
  c.rank.total = r.Dbl();
  std::uint32_t np = r.U32(); if (np > kWireMaxCount) return false;
  for (std::uint32_t i = 0; i < np; ++i) { PathEvidence x; if (!DecodePath(r, x)) return false; c.paths.push_back(x); }
  c.explanation = r.Str();
  return r.ok();
}

inline void EncodeReservation(WireWriter& w, const Reservation& r) {
  w.U64(r.id.Value()); w.U64(r.generation.Value()); w.U64(r.composition.Value());
  w.U32(static_cast<std::uint32_t>(r.items.size()));
  for (const auto& it : r.items) { w.U64(it.resource.Value()); w.U32(static_cast<std::uint32_t>(it.state)); w.Dbl(it.reserved_amount); }
  w.U32(static_cast<std::uint32_t>(r.state));
}
inline bool DecodeReservation(WireReader& rd, Reservation& r) {
  r.id = ReservationId(rd.U64()); r.generation = ReservationGeneration(rd.U64()); r.composition = CompositionId(rd.U64());
  std::uint32_t ni = rd.U32(); if (ni > kWireMaxCount) return false;
  for (std::uint32_t i = 0; i < ni; ++i) { ReservationItem it; it.resource = ResourceId(rd.U64()); std::uint32_t st = rd.U32(); if (!WireValidEnum<ReservationState>(st)) return false; it.state = static_cast<ReservationState>(st); it.reserved_amount = rd.Dbl(); r.items.push_back(it); }
  std::uint32_t st = rd.U32(); if (!WireValidEnum<ReservationState>(st)) return false; r.state = static_cast<ReservationState>(st);
  return rd.ok();
}

}  // namespace dfabric