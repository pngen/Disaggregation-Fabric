// Disaggregation Fabric — backend interface.
#pragma once
#include "dfabric/ids.hpp"
#include "dfabric/enums.hpp"
#include "dfabric/model.hpp"
#include <memory>
#include <vector>
#include <string>
#include <optional>

namespace dfabric {

// A backend provides discovery (nodes/resources) and path evidence. It never
// performs composition or reservation — that work belongs to the engine only.
class Backend {
 public:
  virtual ~Backend() = default;
  virtual BackendKind kind() const noexcept = 0;
  virtual std::string name() const = 0;
  virtual std::vector<Node> DiscoverNodes() = 0;
  virtual std::vector<Resource> DiscoverResources() = 0;
  virtual std::optional<PathEvidence> ResolvePath(const PathRequirement&) = 0;
  virtual bool SupportsRealExecution() const { return false; }
  virtual std::string Describe() const { return name(); }
};

using BackendPtr = std::shared_ptr<Backend>;

}  // namespace dfabric
