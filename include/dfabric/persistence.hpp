// Disaggregation Fabric — versioned, integrity-checked persistence.
#pragma once
#include "dfabric/ids.hpp"
#include "dfabric/model.hpp"
#include "dfabric/composition.hpp"
#include <vector>
#include <string>
#include <map>

namespace dfabric {
namespace persist {

// Durable structural state distilled from the engine. Live worker authority is
// NOT stored as current; dynamic evidence is re-labelled on load.
struct DurableState {
  CoordinatorEpoch epoch{};
  std::vector<Node> nodes;
  std::vector<Resource> resources;
  std::map<PolicyId, PolicyGeneration> policies;
  std::vector<Composition> compositions;   // history; authority not trusted on load
};

bool Save(const DurableState& state, const std::string& path, std::string& err);
bool Load(const std::string& path, DurableState& out, std::string& err);

}  // namespace persist
}  // namespace dfabric
