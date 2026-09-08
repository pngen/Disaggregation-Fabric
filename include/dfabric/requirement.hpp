// Disaggregation Fabric — composition requirement model.
#pragma once
#include "dfabric/ids.hpp"
#include "dfabric/enums.hpp"
#include "dfabric/model.hpp"
#include <set>
#include <vector>
#include <string>
#include <map>

namespace dfabric {

// Placement / shared-risk constraints the consumer may impose.
enum class TopologyConstraint : std::uint32_t {
  NONE = 0,
  COLOCATE = 1,
  SPREAD = 2,
  DISTINCT_FAILURE_DOMAINS = 3,
  MAX_SHARED_RISK = 4,
};

constexpr const char* ToString(TopologyConstraint v) {
  switch (v) {
    case TopologyConstraint::NONE: return "NONE";
    case TopologyConstraint::COLOCATE: return "COLOCATE";
    case TopologyConstraint::SPREAD: return "SPREAD";
    case TopologyConstraint::DISTINCT_FAILURE_DOMAINS: return "DISTINCT_FAILURE_DOMAINS";
    case TopologyConstraint::MAX_SHARED_RISK: return "MAX_SHARED_RISK";
  }
  return "NONE";
}

struct FailureDomainConstraint {
  TopologyConstraint topology = TopologyConstraint::NONE;
  std::uint32_t max_shared_risk = 0;   // meaningful when topology == MAX_SHARED_RISK
  std::vector<FailureDomainType> allowed_types;  // empty => any type
};

// A consumer expresses the required resource composition. Hard requirements
// must remain hard — the engine never silently weakens them.
struct CompositionRequest {
  std::uint64_t compute_units = 0;
  std::uint32_t accelerator_count = 0;
  std::vector<std::string> accelerator_architectures;  // allowed accelerator arch
  double accelerator_memory_bytes = 0.0;
  double host_memory_bytes = 0.0;
  double expanded_memory_bytes = 0.0;
  double storage_bytes = 0.0;
  std::uint32_t network_endpoint_count = 0;
  double min_bandwidth_mbps = 0.0;
  double max_latency_us = 0.0;

  std::set<ResourceClass> mandatory_classes;
  std::set<ResourceClass> optional_classes;

  bool require_direct_path = false;
  bool require_coherence = false;
  bool require_persistence = false;
  bool require_isolation = false;
  bool staging_allowance = false;
  bool ignore_reachability = false;  // test/dev only; never set by normal consumers

  CompatibilityId compatibility{};
  FailureDomainConstraint failure_domain;
  PolicyId policy{};
  PolicyGeneration policy_generation{};
  ConsumerId consumer{};
  ConsumerGeneration consumer_generation{};

  std::map<std::string, double> constraints;  // extra numeric constraints
};

}  // namespace dfabric
