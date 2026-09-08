// Disaggregation Fabric — composition graph model.
#pragma once
#include "dfabric/ids.hpp"
#include "dfabric/enums.hpp"
#include "dfabric/model.hpp"
#include <vector>
#include <string>
#include <algorithm>
#include <set>

namespace dfabric {

// Relationship kinds that a composition edge may carry.
enum class GraphEdgeKind : std::uint32_t {
  REACHABILITY = 1,
  LATENCY_BOUND = 2,
  BANDWIDTH_BOUND = 3,
  COMPATIBILITY = 4,
  PROTOCOL = 5,
  LOCALITY = 6,
  FRESHNESS = 7,
  GENERATION = 8,
};

template <> struct EnumTraits<GraphEdgeKind> { static constexpr GraphEdgeKind Min() noexcept { return GraphEdgeKind::REACHABILITY; } static constexpr std::uint32_t MaxValue = 8; };

constexpr const char* ToString(GraphEdgeKind v) {
  switch (v) {
    case GraphEdgeKind::REACHABILITY: return "REACHABILITY";
    case GraphEdgeKind::LATENCY_BOUND: return "LATENCY_BOUND";
    case GraphEdgeKind::BANDWIDTH_BOUND: return "BANDWIDTH_BOUND";
    case GraphEdgeKind::COMPATIBILITY: return "COMPATIBILITY";
    case GraphEdgeKind::PROTOCOL: return "PROTOCOL";
    case GraphEdgeKind::LOCALITY: return "LOCALITY";
    case GraphEdgeKind::FRESHNESS: return "FRESHNESS";
    case GraphEdgeKind::GENERATION: return "GENERATION";
  }
  return "UNKNOWN";
}

// A directed relationship between two resource instances in a composition.
// A composition is valid only if every mandatory edge constraint passes.
struct GraphEdge {
  ResourceId from{};
  ResourceId to{};
  GraphEdgeKind kind = GraphEdgeKind::REACHABILITY;
  bool required = true;
  double min_bandwidth_mbps = 0.0;   // for BANDWIDTH_BOUND
  double max_latency_us = 0.0;       // for LATENCY_BOUND
  std::string protocol;              // for PROTOCOL / COMPATIBILITY
  std::string compat_facet;          // for COMPATIBILITY (name of pin facet)
  bool operator==(const GraphEdge&) const = default;
};

// A resource set modelled as a graph: vertices are resources, edges are the
// required cross-resource relationships. Individually valid resources do not
// form a valid set if their edges are invalid.
struct ResourceGraph {
  std::vector<ResourceId> vertices;
  std::vector<GraphEdge> edges;

  bool HasVertex(ResourceId id) const {
    return std::find(vertices.begin(), vertices.end(), id) != vertices.end();
  }
  void AddVertex(ResourceId id) {
    if (!HasVertex(id)) vertices.push_back(id);
  }
  void AddEdge(const GraphEdge& e) { edges.push_back(e); }
  bool operator==(const ResourceGraph&) const = default;
};

}  // namespace dfabric