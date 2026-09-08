// Disaggregation Fabric — fabric engine (public API).
#pragma once
#include "dfabric/ids.hpp"
#include "dfabric/enums.hpp"
#include "dfabric/model.hpp"
#include "dfabric/graph.hpp"
#include "dfabric/requirement.hpp"
#include "dfabric/composition.hpp"
#include "dfabric/backend.hpp"
#include <memory>
#include <optional>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <cstdint>

namespace dfabric {

struct EligibilityDetail {
  bool eligible = false;
  std::vector<RejectionReason> reasons;
  std::vector<std::string> notes;
  bool ok() const noexcept { return eligible; }
};

struct ComposeResult {
  CompositionOutcome outcome = CompositionOutcome::REJECTED;
  CompositionPlan plan;
  std::vector<RejectionReason> reasons;
  std::vector<std::string> notes;
  bool ok() const noexcept { return outcome == CompositionOutcome::SUCCESS; }
};

struct ReserveResult {
  CompositionOutcome outcome = CompositionOutcome::REJECTED;
  Reservation reservation;
  std::vector<RejectionReason> reasons;
  std::vector<std::string> notes;
  bool ok() const noexcept { return outcome == CompositionOutcome::SUCCESS; }
};

struct CommitResult {
  CompositionOutcome outcome = CompositionOutcome::REJECTED;
  Composition composition;
  std::vector<RejectionReason> reasons;
  std::vector<std::string> notes;
  bool ok() const noexcept { return outcome == CompositionOutcome::SUCCESS; }
};

struct ActivateResult {
  CompositionOutcome outcome = CompositionOutcome::REJECTED;
  Composition composition;
  std::vector<RejectionReason> reasons;
  std::vector<std::string> notes;
  bool ok() const noexcept { return outcome == CompositionOutcome::SUCCESS; }
};

struct RevalidateResult {
  bool valid = false;
  std::vector<RejectionReason> reasons;
  std::vector<std::string> notes;
  bool ok() const noexcept { return valid; }
};

struct DiscoveryResult {
  std::vector<Node> nodes;
  std::vector<Resource> resources;
};

// A bounded snapshot of the registry (for inspection/CLI).
struct RegistrySnapshot {
  std::vector<Node> nodes;
  std::vector<Resource> resources;
  std::vector<Composition> compositions;
  std::vector<Reservation> reservations;
  CoordinatorEpoch epoch{};
};

struct EngineConfig {
  CoordinatorId coordinator{};
  CoordinatorEpoch epoch{};
  std::string node_authority;   // hostname / authority label
  bool persist_authority_only = true;
};

struct LoadResult {
  bool ok = false;
  std::string error;
};

// The FabricEngine is a thread-safe, vendor-neutral component decision engine.
// It composes separated resources into generation-bound resource sets. All
// mutating operations are serialized under a single lock; long-running backend
// waits/CUDA sync/network I/O must NOT be performed under the engine lock.
class FabricEngine {
 public:
  explicit FabricEngine(EngineConfig cfg);
  ~FabricEngine();
  FabricEngine(const FabricEngine&) = delete;
  FabricEngine& operator=(const FabricEngine&) = delete;
  FabricEngine(FabricEngine&&) = delete;
  FabricEngine& operator=(FabricEngine&&) = delete;

  // --- discovery / ingestion -------------------------------------------
  void AddBackend(BackendPtr b);
  DiscoveryResult Discover();                 // ingest nodes/resources from backends
  bool RegisterNode(const Node& n);           // worker/backend published
  bool RegisterResource(const Resource& r);   // worker/backend published
  void InvalidateNode(ResourceId);            // mark resource revalidation required
  void WithdrawResource(ResourceId);          // offline + withdraw
  void MarkEvidenceRevalidationRequired(WorkerBootId owner);  // worker dead
  void AdvanceEpoch(CoordinatorEpoch next);   // move to a fresh epoch

  // --- metadata / queries ---------------------------------------------
  std::size_t NodeCount() const;
  std::size_t ResourceCount() const;
  std::size_t CompositionCount() const;
  std::optional<Resource> GetResource(ResourceId) const;
  std::optional<Node> GetNode(NodeId) const;
  std::optional<Composition> GetComposition(CompositionId) const;
  std::optional<Reservation> GetReservation(ReservationId) const;
  RegistrySnapshot Snapshot() const;
  CoordinatorEpoch epoch() const;
  std::string ExplainEligibility(ResourceId, const CompositionRequest&) const;
  std::string ExplainCompositionResult(const ComposeResult&) const;

  // --- eligibility / plan / reserve / commit / activate ---------------
  EligibilityDetail CheckEligibility(ResourceId, const CompositionRequest&) const;
  ComposeResult Compose(const CompositionRequest& req);
  ComposeResult ComposeDeterministic(const CompositionRequest& req, std::uint64_t seed);
  ReserveResult Reserve(const CompositionPlan& plan);
  RevalidateResult Revalidate(const Composition& c) const;
  CommitResult Commit(const Reservation& res);
  ActivateResult Activate(CompositionId id);

  // --- invalidation / lifecycle ---------------------------------------
  void OnResourceUpdate(const Resource& updated);   // generation change
  void OnNodeUpdate(const Node& updated);
  void OnPolicyUpdate(PolicyId, PolicyGeneration);
  void OnPathUpdate(const PathEvidence&);
  void OnResourceFailure(ResourceId);
  bool BeginDrain(CompositionId);
  bool EndDrain(CompositionId);
  bool Release(CompositionId);
  bool Recompute(CompositionId);   // attempt recomposition of a degraded comp

  // --- persistence -----------------------------------------------------
  bool Save(const std::string& path) const;
  LoadResult Load(const std::string& path);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  void IngestNodeLocked(const Node& n);
  void IngestResourceLocked(const Resource& r);
};

}  // namespace dfabric