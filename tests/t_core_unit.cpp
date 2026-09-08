#include "dfabric/engine.hpp"
#include <cassert>
#include <cstdio>

using namespace dfabric;

int main() {
  EngineConfig cfg;
  cfg.coordinator = CoordinatorId(7);
  cfg.epoch = CoordinatorEpoch(1);
  FabricEngine e(cfg);
  assert(e.epoch() == CoordinatorEpoch(1));
  assert(e.ResourceCount() == 0);
  printf("core unit smoke ok\n");
  return 0;
}