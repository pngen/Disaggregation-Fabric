// Disaggregation Fabric — authority binding.
#pragma once
#include "dfabric/ids.hpp"
#include <vector>

namespace dfabric {

// The full set of generations that a composition is bound to. If any of these
// change, the composition must become REVALIDATION_REQUIRED / FENCED / INVALID
// or RECOMPOSITION_REQUIRED depending on lifecycle.
struct AuthorityTuple {
  CoordinatorEpoch coordinator_epoch{};
  std::vector<WorkerBootId> worker_boots;    // all process-owned members
  std::vector<NodeGeneration> node_generations;
  std::vector<ResourceGeneration> resource_generations;
  std::vector<PathGeneration> path_generations;
  std::vector<CapabilityGeneration> capability_generations;
  std::vector<CompatibilityGeneration> compatibility_generations;
  PolicyGeneration policy_generation{};
  std::vector<EvidenceGeneration> evidence_generations;
  std::vector<ReservationGeneration> reservation_generations;
  CompositionGeneration composition_generation{};
};

}  // namespace dfabric
