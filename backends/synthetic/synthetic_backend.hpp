// Disaggregation Fabric — synthetic multi-node backend (SYNTHETIC).
// Models physically-separated nodes with explicit cross-node relationship and
// failure semantics. All multi-node topology in this backend is SYNTHETIC.
#pragma once
#include "dfabric/backend.hpp"
#include "dfabric/model.hpp"
#include "dfabric/requirement.hpp"
#include <mutex>

namespace dfabric {

class SyntheticBackend : public Backend {
 public:
  SyntheticBackend(NodeGeneration node_gen, CompositionRequest baseline_req);
  ~SyntheticBackend() override = default;

  BackendKind kind() const noexcept override { return BackendKind::SYNTHETIC; }
  std::string name() const override { return "synthetic"; }
  std::vector<Node> DiscoverNodes() override;
  std::vector<Resource> DiscoverResources() override;
  std::optional<PathEvidence> ResolvePath(const PathRequirement&) override;
  bool SupportsRealExecution() const override { return false; }
  std::string Describe() const override;

  // --- simulation control (for proof tests) ------------------------------
  void SetResourceHealth(ResourceId, ResourceHealth);
  void SetResourceReachability(ResourceId, Reachability);
  void SetResourceGeneration(ResourceId, ResourceGeneration);
  void SetResourceUnavailable(ResourceId);
  void SetResourceBack(ResourceId);
  void SetNodeHealth(NodeId, ResourceHealth);
  void SetPathAvailable(NodeId, NodeId, bool available);
  void SetPathBandwidth(NodeId, NodeId, double mbps);
  void SetPathLatency(NodeId, NodeId, double us);
  void RemoveNode(NodeId);
  void ReincarnateNode(NodeId, NodeGeneration, WorkerBootId);
  std::string Dump() const;

 private:
  mutable std::mutex mu_;
  std::vector<Node> nodes_;
  std::vector<Resource> resources_;
  struct PathState { bool available; double bw; double lat; PathGeneration gen; };
  std::map<std::string, PathState> paths_;
  NodeGeneration node_gen_;
};

}  // namespace dfabric
