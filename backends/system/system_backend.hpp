// Disaggregation Fabric — real single-host system backend (REAL).
// Discovers the actual machine: CPU, NUMA, host memory, storage, and a network
// endpoint using real OS APIs. Physical multi-node disaggregation is UNSUPPORTED
// on this host. No hardware is fabricated.
#pragma once
#include "dfabric/backend.hpp"
#include "dfabric/model.hpp"

namespace dfabric {

class SystemBackend : public Backend {
 public:
  SystemBackend();
  BackendKind kind() const noexcept override { return BackendKind::REAL; }
  std::string name() const override { return "system"; }
  std::vector<Node> DiscoverNodes() override;
  std::vector<Resource> DiscoverResources() override;
  std::optional<PathEvidence> ResolvePath(const PathRequirement&) override;
  bool SupportsRealExecution() const override { return true; }
  std::string Describe() const override;
 private:
  Node node_;
  std::vector<Resource> res_;
};
}
