// Disaggregation Fabric — composition plan / composition / reservation models.
#pragma once
#include "dfabric/ids.hpp"
#include "dfabric/enums.hpp"
#include "dfabric/graph.hpp"
#include "dfabric/requirement.hpp"
#include "dfabric/authority.hpp"
#include "dfabric/model.hpp"
#include <vector>
#include <string>

namespace dfabric {

struct RankingFactor {
  std::string name;
  double weight = 1.0;
  double value = 0.0;    // normalized contribution
  std::string detail;
};

struct RankScore {
  double total = 0.0;
  std::vector<RankingFactor> factors;
};

struct CompositionPlan {
  CompositionId id{};
  CompositionGeneration generation{};
  ResourceGraph graph;
  std::vector<NodeId> nodes;
  CompositionRequest request;
  RankScore rank;
  AuthorityTuple authority;
  CompositionLifecycle lifecycle = CompositionLifecycle::PLANNED;
  std::vector<PathEvidence> paths;   // path evidence used
};

struct Composition {
  CompositionId id{};
  CompositionGeneration generation{};
  ResourceSetId resource_set_id{};
  ResourceSetGeneration resource_set_generation{};
  ResourceGraph graph;
  std::vector<NodeId> nodes;
  CompositionRequest request;
  AuthorityTuple authority;
  CompositionLifecycle lifecycle = CompositionLifecycle::REQUESTED;
  RankScore rank;
  ReservationId reservation{};
  std::vector<PathEvidence> paths;
  CompositionOutcome outcome = CompositionOutcome::OUTCOME_UNKNOWN;
  std::string explanation;
};

struct ReservationItem {
  ResourceId resource{};
  ReservationState state = ReservationState::NONE;
  double reserved_amount = 0.0;
};

struct Reservation {
  ReservationId id{};
  ReservationGeneration generation{};
  CompositionId composition{};
  std::vector<ReservationItem> items;
  ReservationState state = ReservationState::NONE;
};

}  // namespace dfabric
