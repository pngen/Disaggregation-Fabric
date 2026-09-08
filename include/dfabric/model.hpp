// Disaggregation Fabric — resource / node / capability / evidence model.
#pragma once
#include "dfabric/ids.hpp"
#include "dfabric/enums.hpp"
#include <map>
#include <vector>
#include <string>
#include <optional>
#include <cstdint>

namespace dfabric {

// Aggregate capacity a resource exposes or a requirement demands. Only the
// quantities Disaggregation Fabric actually reasons about are first-class; the
// `extra` map carries backend/provider-specific quantities.
struct Capacity {
  double compute_units = 0.0;
  double accelerator_memory_bytes = 0.0;
  double host_memory_bytes = 0.0;
  double expanded_memory_bytes = 0.0;
  double storage_bytes = 0.0;
  double network_bandwidth_mbps = 0.0;   // bandwidth
  double network_latency_us = 0.0;       // latency (lower is better)
  std::map<std::string, double> extra;   // generic extension dimensions
  bool operator==(const Capacity&) const = default;
};

// A single explicit capability claim. UNKNOWN must fail closed; never promote
// UNKNOWN to SUPPORTED implicitly.
struct Capability {
  CapabilityKind kind = CapabilityKind::UNKNOWN;
  CapabilityState state = CapabilityState::UNKNOWN;
  CapabilityGeneration generation{};
  EvidenceId evidence{};
  std::string detail;   // e.g. "12.0" for ARCH_GENERATION
};

// Compatibility pin: a named compatibility facet and its expected value.
struct CompatibilityPin {
  std::string name;
  std::string value;
  CompatibilityGeneration generation{};
  bool operator==(const CompatibilityPin&) const = default;
};

struct Compatibility {
  CompatibilityId id{};
  CompatibilityGeneration generation{};
  std::vector<CompatibilityPin> pins;
};

// Failure domain: only claim domain knowledge when evidence exists. Synthetic
// domains are explicitly labelled.
struct FailureDomain {
  FailureDomainId id{};
  FailureDomainType type = FailureDomainType::UNKNOWN;
  std::string name;              // e.g. "rack-07", "socket-1"
  bool synthetic = false;        // must be true for synthetic/multi-node models
  std::map<std::string, double> locality_coords;
};

// Dynamic evidence record. Authority owner identifies who owns the evidence
// state (a WorkerBootId string / provider / backend).
struct Evidence {
  EvidenceId id{};
  EvidenceGeneration generation{};
  EvidenceSource source = EvidenceSource::UNKNOWN;
  EvidenceState state = EvidenceState::PROVENANCE_UNKNOWN;  // freshness
  std::uint64_t timestamp_ms = 0;
  std::uint64_t max_age_ms = 0;    // accepted age; 0 => revalidation required
  double confidence = 0.0;         // [0,1]
  std::string provenance;
  std::string owner;               // WorkerBootId string or "coordinator"/"backend"
  bool durable = false;            // true only if independently durable, not live worker authority

  bool IsCurrent() const noexcept {
    if (state == EvidenceState::STALE || state == EvidenceState::REVALIDATION_REQUIRED) return false;
    if (max_age_ms == 0) return false;  // must be revalidated
    return true;
  }
};

// Locality coordinate vector (unitless), used for distance estimation.
struct Locality {
  std::vector<double> coords;
  std::map<std::string, double> tags;   // e.g. { "numa_node": 0.0 }
  bool operator==(const Locality&) const = default;
};

struct Resource {
  ResourceId id{};
  ResourceGeneration generation{};
  ResourceClass resource_class = ResourceClass::UNKNOWN;
  NodeId node_id{};
  std::string provider;
  std::vector<Capability> capabilities;
  std::vector<Compatibility> compatibility;
  Capacity capacity;
  ResourceHealth health = ResourceHealth::UNKNOWN;
  Readiness readiness = Readiness::UNKNOWN;
  Reachability reachability = Reachability::UNKNOWN;
  Locality locality;
  FailureDomain failure_domain;
  Evidence evidence;
  std::string authority_owner;   // e.g. WorkerBootId string / provider
  bool synthetic = false;
};

struct Node {
  NodeId id{};
  NodeGeneration generation{};
  std::string hostname;   // or opaque node label
  std::string provider;
  BackendKind backend = BackendKind::UNKNOWN;
  std::vector<ResourceId> inventory;   // resources hosted by this node
  FailureDomain failure_domain;
  Locality locality;
  Reachability reachability = Reachability::UNKNOWN;
  ResourceHealth health = ResourceHealth::UNKNOWN;
  std::string worker_owner;            // WorkerId string, "" if none
  WorkerBootId worker_boot{};
  std::uint64_t evidence_freshness_ms = 0;
  NodeLifecycle lifecycle = NodeLifecycle::DISCOVERED;
  bool synthetic = false;
};

// A path requirement / evidence pair consumed by the engine. The engine may
// require paths but never invents connectivity.
struct PathRequirement {
  NodeId from{};
  NodeId to{};
  double min_bandwidth_mbps = 0.0;
  double max_latency_us = 0.0;
  bool required = false;   // hard requirement
  bool direct = false;     // direct-path requirement
  std::string protocol;
};

struct PathEvidence {
  PathEvidenceId id{};
  PathGeneration generation{};
  PathRequirement requirement;
  bool available = false;
  double bandwidth_mbps = 0.0;
  double latency_us = 0.0;
  bool verified = false;   // must be verified to be usable
  std::string provider;
  std::string source;
  Evidence evidence;
};

}  // namespace dfabric
