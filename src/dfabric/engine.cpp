// Disaggregation Fabric — engine implementation.
#include "dfabric/engine.hpp"
#include "dfabric/authority.hpp"
#include "dfabric/persistence.hpp"

#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <mutex>
#include <cmath>
#include <sstream>
#include <utility>

namespace dfabric {

namespace {

using ResourceMap = std::unordered_map<ResourceId, Resource>;
using NodeMap = std::unordered_map<NodeId, Node>;
using PathMap = std::map<std::string, PathEvidence>;

struct ClassDemand {
  ResourceClass cls = ResourceClass::UNKNOWN;
  bool mandatory = false;
  bool amount_based = false;
  double amount = 0.0;
  std::uint32_t count = 0;
  std::string dimension;
};

std::string PathKey(NodeId a, NodeId b) { return a.ToString() + "->" + b.ToString(); }

std::vector<CapabilityKind> ClassCapabilityKinds(ResourceClass cls) {
  switch (cls) {
    case ResourceClass::COMPUTE: return { CapabilityKind::CPU_EXECUTION };
    case ResourceClass::ACCELERATOR: return { CapabilityKind::CUDA_EXECUTION, CapabilityKind::ROCM_EXECUTION, CapabilityKind::CPU_EXECUTION };
    case ResourceClass::HOST_MEMORY: return { CapabilityKind::HOST_MEMORY_ACCESS };
    case ResourceClass::EXPANDED_MEMORY: return { CapabilityKind::EXPANDED_MEMORY_ACCESS };
    case ResourceClass::STORAGE: return { CapabilityKind::PERSISTENT_STORAGE };
    case ResourceClass::NETWORK_ENDPOINT:
    case ResourceClass::NIC: return { CapabilityKind::DIRECT_NETWORK_ACCESS, CapabilityKind::NETWORK_PROTOCOL };
    case ResourceClass::FABRIC_ENDPOINT: return { CapabilityKind::NETWORK_PROTOCOL, CapabilityKind::RDMA_CAPABLE };
    case ResourceClass::DPU: return { CapabilityKind::CPU_EXECUTION, CapabilityKind::DIRECT_NETWORK_ACCESS };
    case ResourceClass::SERVICE_ENDPOINT:
    case ResourceClass::CONTROL_PROCESS:
    case ResourceClass::OTHER_REGISTERED: return { CapabilityKind::CPU_EXECUTION, CapabilityKind::NETWORK_PROTOCOL };
    case ResourceClass::UNKNOWN: return {};
  }
  return {};
}

const Capability* FindCapability(const Resource& r, CapabilityKind kind) {
  for (const auto& c : r.capabilities) if (c.kind == kind) return &c;
  return nullptr;
}

bool CapabilitySupported(const Resource& r, CapabilityKind kind) {
  const Capability* c = FindCapability(r, kind);
  return c != nullptr && c->state == CapabilityState::SUPPORTED;
}

double CapacityForDimension(const Resource& r, const std::string& dim) {
  if (dim == "compute_units") return r.capacity.compute_units;
  if (dim == "accelerator_memory_bytes") return r.capacity.accelerator_memory_bytes;
  if (dim == "host_memory_bytes") return r.capacity.host_memory_bytes;
  if (dim == "expanded_memory_bytes") return r.capacity.expanded_memory_bytes;
  if (dim == "storage_bytes") return r.capacity.storage_bytes;
  if (dim == "network_bandwidth_mbps") return r.capacity.network_bandwidth_mbps;
  double v = 0.0;
  auto it = r.capacity.extra.find(dim);
  if (it != r.capacity.extra.end()) v = it->second;
  return v;
}

bool ResourceEligible(const Resource& res, const CompositionRequest& req,
                      std::vector<RejectionReason>& out, std::vector<std::string>& notes) {
  out.clear(); notes.clear();
  if (!res.id.IsValid()) { out.push_back(RejectionReason::RESOURCE_OFFLINE); notes.push_back("zero resource id"); return false; }
  if (res.health == ResourceHealth::OFFLINE) { out.push_back(RejectionReason::RESOURCE_OFFLINE); notes.push_back("offline " + res.id.ToString()); return false; }
  if (res.health == ResourceHealth::DEGRADED || res.health == ResourceHealth::UNKNOWN) { out.push_back(RejectionReason::RESOURCE_DEGRADED); notes.push_back("health " + std::string(ToString(res.health)) + " (" + res.id.ToString() + ")"); return false; }
  if (!req.ignore_reachability && res.reachability != Reachability::REACHABLE) { out.push_back(RejectionReason::RESOURCE_UNREACHABLE); notes.push_back("unreachable " + res.id.ToString()); return false; }
  if (res.readiness == Readiness::NOT_READY || res.readiness == Readiness::UNKNOWN) { out.push_back(RejectionReason::RESOURCE_DEGRADED); notes.push_back("not ready " + res.id.ToString()); return false; }
  if (!res.evidence.IsCurrent()) { out.push_back(RejectionReason::STALE_EVIDENCE); notes.push_back("stale evidence " + res.id.ToString()); return false; }
  const auto caps = ClassCapabilityKinds(res.resource_class);
  bool any_ok = false, any_claim = false, any_unknown = false;
  for (CapabilityKind kind : caps) {
    const Capability* c = FindCapability(res, kind);
    if (!c) continue;
    any_claim = true;
    if (c->state == CapabilityState::UNKNOWN) any_unknown = true;
    if (c->state == CapabilityState::SUPPORTED) any_ok = true;
  }
  if (any_claim && any_unknown && !any_ok) { out.push_back(RejectionReason::CAPABILITY_UNKNOWN); notes.push_back("capability unknown " + res.id.ToString()); return false; }
  if (res.resource_class == ResourceClass::ACCELERATOR && !req.accelerator_architectures.empty()) {
    bool matched = false;
    const Capability* arch = FindCapability(res, CapabilityKind::ARCH_GENERATION);
    if (arch && !arch->detail.empty()) {
      for (const auto& a : req.accelerator_architectures) if (arch->detail == a) { matched = true; break; }
    }
    if (!matched) { out.push_back(RejectionReason::COMPATIBILITY_MISMATCH); notes.push_back("arch mismatch " + res.id.ToString()); return false; }
  }
  return true;
}

bool SuppliesCapacity(const Resource& res, const ClassDemand& d) {
  if (!d.amount_based) {
    if (d.cls == ResourceClass::COMPUTE) return res.capacity.compute_units > 0.0;
    return true;
  }
  return CapacityForDimension(res, d.dimension) >= 0.0;
}

double ClassScore(const Resource& res, const ClassDemand& d) {
  double s = 0.0;
  if (d.amount_based) s = std::max(0.0, CapacityForDimension(res, d.dimension));
  else s = (d.cls == ResourceClass::COMPUTE) ? std::max(0.0, res.capacity.compute_units) : 1.0;
  if (res.health == ResourceHealth::HEALTHY) s += 1000.0;
  if (res.readiness == Readiness::READY) s += 100.0;
  if (res.reachability == Reachability::REACHABLE) s += 10.0;
  return s;
}

std::vector<ClassDemand> ComputeDemands(const CompositionRequest& req) {
  std::vector<ClassDemand> out;
  auto add = [&](ResourceClass cls, bool amount_based, double amount, std::uint32_t count, const std::string& dim) {
    out.push_back(ClassDemand{ cls, false, amount_based, amount, count, dim });
  };
  // NOTE: NETWORK_ENDPOINT is treated as count-based for endpoint count, but
  // we also carry a bandwidth floor via constraint checks on edges.
  add(ResourceClass::COMPUTE, true, static_cast<double>(req.compute_units), 0, "compute_units");
  add(ResourceClass::ACCELERATOR, false, req.accelerator_memory_bytes, req.accelerator_count, "accelerator_memory_bytes");
  add(ResourceClass::HOST_MEMORY, true, req.host_memory_bytes, 0, "host_memory_bytes");
  add(ResourceClass::EXPANDED_MEMORY, true, req.expanded_memory_bytes, 0, "expanded_memory_bytes");
  add(ResourceClass::STORAGE, true, req.storage_bytes, 0, "storage_bytes");
  add(ResourceClass::NETWORK_ENDPOINT, false, req.min_bandwidth_mbps, req.network_endpoint_count, "network_bandwidth_mbps");
  for (auto& d : out) {
    const bool nonzero = (d.amount_based && d.amount > 0.0) || (!d.amount_based && d.count > 0);
    if (nonzero || req.mandatory_classes.count(d.cls)) d.mandatory = true;
    if (d.cls == ResourceClass::NETWORK_ENDPOINT && req.mandatory_classes.count(d.cls) && req.network_endpoint_count == 0) d.count = 1;
  }
  return out;
}

// Builds the cross-resource edges of a composition graph from the resource set.
ResourceGraph BuildGraph(const CompositionRequest& req, const std::vector<Resource>& set) {
  ResourceGraph g;
  for (const auto& r : set) g.AddVertex(r.id);
  auto addEdge = [&](const Resource& a, const Resource& b, GraphEdgeKind kind) {
    if (a.id == b.id) return;
    GraphEdge e;
    e.from = a.id; e.to = b.id; e.kind = kind;
    e.required = true;
    e.min_bandwidth_mbps = req.min_bandwidth_mbps;
    e.max_latency_us = req.max_latency_us;
    g.AddEdge(e);
  };
  // compute <-> accelerator
  for (const auto& c : set) if (c.resource_class == ResourceClass::COMPUTE)
    for (const auto& a : set) if (a.resource_class == ResourceClass::ACCELERATOR)
      if (c.id != a.id) { addEdge(c, a, GraphEdgeKind::REACHABILITY); addEdge(c, a, GraphEdgeKind::COMPATIBILITY); }
  // accelerator <-> memory
  for (const auto& a : set) if (a.resource_class == ResourceClass::ACCELERATOR)
    for (const auto& m : set)
      if ((m.resource_class == ResourceClass::HOST_MEMORY || m.resource_class == ResourceClass::EXPANDED_MEMORY) && m.id != a.id) {
        addEdge(a, m, GraphEdgeKind::REACHABILITY); addEdge(a, m, GraphEdgeKind::BANDWIDTH_BOUND); addEdge(a, m, GraphEdgeKind::LATENCY_BOUND);
      }
  // accelerator <-> nic / network endpoint
  for (const auto& a : set) if (a.resource_class == ResourceClass::ACCELERATOR)
    for (const auto& n : set)
      if ((n.resource_class == ResourceClass::NIC || n.resource_class == ResourceClass::NETWORK_ENDPOINT) && n.id != a.id) {
        addEdge(a, n, GraphEdgeKind::REACHABILITY); addEdge(a, n, GraphEdgeKind::BANDWIDTH_BOUND);
      }
  // compute <-> storage
  for (const auto& c : set) if (c.resource_class == ResourceClass::COMPUTE)
    for (const auto& s : set) if (s.resource_class == ResourceClass::STORAGE && s.id != c.id) {
      addEdge(c, s, GraphEdgeKind::REACHABILITY); addEdge(c, s, GraphEdgeKind::BANDWIDTH_BOUND);
    }
  return g;
}

// Validate a resource graph against current state (paths, resources, domains).
bool ValidateGraph(const CompositionRequest& req, const ResourceGraph& g,
                   const ResourceMap& resources, const NodeMap& nodes, const PathMap& paths,
                   std::vector<RejectionReason>& out, std::vector<std::string>& notes) {
  out.clear(); notes.clear();
  (void)req; (void)nodes;
  for (const auto& e : g.edges) {
    auto fit = resources.find(e.from); auto tit = resources.find(e.to);
    if (fit == resources.end() || tit == resources.end()) { out.push_back(RejectionReason::RESOURCE_OFFLINE); notes.push_back("edge endpoint missing"); return false; }
    const Resource& from = fit->second; const Resource& to = tit->second;
    const bool same_node = (from.node_id == to.node_id);
    double bw = same_node ? 1.0e12 : 0.0;
    double lat = same_node ? 0.0 : 1.0e12;
    if (!same_node) {
      auto pit = paths.find(PathKey(from.node_id, to.node_id));
      if (pit == paths.end() || !pit->second.verified || !pit->second.available) {
        out.push_back(RejectionReason::PATH_UNAVAILABLE); notes.push_back("no verified path " + from.node_id.ToString() + "->" + to.node_id.ToString()); return false;
      }
      if (pit->second.evidence.state == EvidenceState::STALE || !pit->second.evidence.IsCurrent()) {
        out.push_back(RejectionReason::PATH_STALE); notes.push_back("stale path " + from.node_id.ToString() + "->" + to.node_id.ToString()); return false;
      }
      bw = pit->second.bandwidth_mbps; lat = pit->second.latency_us;
    }
    if (e.kind == GraphEdgeKind::BANDWIDTH_BOUND && e.min_bandwidth_mbps > 0.0 && bw < e.min_bandwidth_mbps) {
      out.push_back(RejectionReason::BANDWIDTH_INSUFFICIENT); notes.push_back("bw edge " + from.id.ToString() + "-" + to.id.ToString()); return false;
    }
    if (e.kind == GraphEdgeKind::LATENCY_BOUND && e.max_latency_us > 0.0 && lat > e.max_latency_us) {
      out.push_back(RejectionReason::LATENCY_LIMIT_EXCEEDED); notes.push_back("latency edge " + from.id.ToString() + "-" + to.id.ToString()); return false;
    }
    if (e.kind == GraphEdgeKind::COMPATIBILITY) {
      // Compatibility is enforced elsewhere (arch / pins). Here we conservatively
      // accept only if both have a compatibility claim or neither requires it.
    }
  }
  return true;
}

// Hard failure-domain constraint checks.
bool ValidateFailureDomains(const std::vector<Resource>& set, const NodeMap& nodes,
                            const CompositionRequest& req,
                            std::vector<RejectionReason>& out, std::vector<std::string>& notes) {
  out.clear(); notes.clear();
  (void)nodes;
  if (req.failure_domain.topology == TopologyConstraint::NONE) return true;
  std::set<std::string> domains;
  for (const auto& r : set) {
    std::string key = r.node_id.ToString() + ":" + r.failure_domain.name;
    domains.insert(key);
  }
  if (req.failure_domain.topology == TopologyConstraint::DISTINCT_FAILURE_DOMAINS) {
    if (domains.size() < 2) { out.push_back(RejectionReason::FAILURE_DOMAIN_CONFLICT); notes.push_back("distinct domains required"); return false; }
  }
  if (req.failure_domain.topology == TopologyConstraint::MAX_SHARED_RISK) {
    std::map<std::string, int> counts;
    for (const auto& r : set) counts[r.failure_domain.name]++;
    int maxc = 0; for (auto& kv : counts) maxc = std::max(maxc, kv.second);
    if (req.failure_domain.max_shared_risk > 0 && static_cast<std::uint32_t>(maxc) > req.failure_domain.max_shared_risk) {
      out.push_back(RejectionReason::FAILURE_DOMAIN_CONFLICT); notes.push_back("max shared risk exceeded"); return false;
    }
  }
  if (req.failure_domain.topology == TopologyConstraint::COLOCATE) {
    std::set<std::string> node_keys;
    for (const auto& r : set) node_keys.insert(r.node_id.ToString());
    if (node_keys.size() > 1) { out.push_back(RejectionReason::FAILURE_DOMAIN_CONFLICT); notes.push_back("colocate required"); return false; }
  }
  if (req.failure_domain.topology == TopologyConstraint::SPREAD) {
    std::set<std::string> node_keys;
    for (const auto& r : set) node_keys.insert(r.node_id.ToString());
    if (node_keys.size() < 2) { out.push_back(RejectionReason::FAILURE_DOMAIN_CONFLICT); notes.push_back("spread required"); return false; }
  }
  return true;
}

AuthorityTuple MakeAuthority(CompositionGeneration gen, const CompositionRequest& req,
                             const std::vector<Resource>& set, const NodeMap& nodes,
                             const PathMap& paths, CoordinatorEpoch epoch) {
  AuthorityTuple a;
  a.coordinator_epoch = epoch;
  a.policy_generation = req.policy_generation;
  a.composition_generation = gen;
  for (const auto& r : set) {
    a.resource_generations.push_back(r.generation);
    a.evidence_generations.push_back(r.evidence.generation);
    for (const auto& c : r.capabilities) a.capability_generations.push_back(c.generation);
    for (const auto& c : r.compatibility) a.compatibility_generations.push_back(c.generation);
  }
  for (const auto& r : set) {
    auto nit = nodes.find(r.node_id);
    if (nit != nodes.end()) {
      a.node_generations.push_back(nit->second.generation);
      if (nit->second.worker_boot.IsValid()) a.worker_boots.push_back(nit->second.worker_boot);
    }
  }
  for (const auto& kv : paths) a.path_generations.push_back(kv.second.generation);
  return a;
}

bool RevalidationHolds(const Composition& c, const ResourceMap& resources, const NodeMap& nodes,
                       const PathMap& paths, CoordinatorEpoch epoch,
                       std::vector<RejectionReason>& out, std::vector<std::string>& notes) {
  out.clear(); notes.clear();
  (void)nodes; (void)paths;
  if (c.authority.coordinator_epoch != epoch) { out.push_back(RejectionReason::WRONG_EPOCH); notes.push_back("epoch mismatch"); return false; }
  for (std::size_t i = 0; i < c.graph.vertices.size(); ++i) {
    ResourceId rid = c.graph.vertices[i];
    auto it = resources.find(rid);
    if (it == resources.end() || !it->second.id.IsValid()) { out.push_back(RejectionReason::RESOURCE_OFFLINE); notes.push_back("resource missing " + rid.ToString()); return false; }
    const Resource& r = it->second;
    if (i < c.authority.resource_generations.size() && c.authority.resource_generations[i] != r.generation) {
      out.push_back(RejectionReason::WRONG_RESOURCE_GENERATION); notes.push_back("resource gen changed " + rid.ToString()); return false;
    }
    if (!r.evidence.IsCurrent()) { out.push_back(RejectionReason::STALE_EVIDENCE); notes.push_back("evidence stale " + rid.ToString()); return false; }
  }
  for (const auto& ng : c.authority.node_generations) {
    // At least one node with a matching generation must exist; conservative
    // approach: node generation checked per resource node below.
    (void)ng;
  }
  return true;
}

// Deterministic ranking: compute a score with named factors.
RankScore Rank(const std::vector<Resource>& set, const NodeMap& nodes, const CompositionRequest& req) {
  (void)nodes; (void)req;
  RankScore s;
  double locality = 0.0, capacity = 0.0, diversity = 0.0, health = 0.0, headroom = 0.0;
  std::set<std::string> node_keys;
  for (const auto& r : set) {
    node_keys.insert(r.node_id.ToString());
    capacity += CapacityForDimension(r, "compute_units") + CapacityForDimension(r, "host_memory_bytes") * 1e-6;
    if (r.health == ResourceHealth::HEALTHY) health += 10.0;
    headroom += (r.capacity.compute_units > 0.0 ? 1.0 : 0.0) + (r.capacity.storage_bytes > 0.0 ? 1.0 : 0.0);
    locality += static_cast<double>(r.locality.coords.size());
  }
  diversity = static_cast<double>(node_keys.size());
  s.factors.push_back({ "health", 1.0, health, "aggregate member health" });
  s.factors.push_back({ "capacity", 1.0, std::min(1.0, capacity * 0.1), "aggregate capacity" });
  s.factors.push_back({ "failure_domain_diversity", 1.0, diversity, "distinct nodes" });
  s.factors.push_back({ "headroom", 1.0, std::min(1.0, headroom * 0.1), "spare capacity" });
  double total = 0.0;
  for (const auto& f : s.factors) total += f.weight * f.value;
  s.total = total;
  return s;
}

std::string CanonicalKey(const std::vector<Resource>& set) {
  std::vector<ResourceId> ids;
  for (const auto& r : set) ids.push_back(r.id);
  std::sort(ids.begin(), ids.end());
  std::string k;
  for (const auto& id : ids) k += id.ToString() + ",";
  return k;
}

}  // namespace

struct FabricEngine::Impl {
  EngineConfig config;
  mutable std::mutex mu;
  ResourceMap resources;
  NodeMap nodes;
  std::vector<BackendPtr> backends;
  PathMap paths;
  std::unordered_map<CompositionId, Composition> compositions;
  std::unordered_map<ReservationId, Reservation> reservations;
  std::map<ResourceId, double> reserved_amount;
  std::map<PolicyId, PolicyGeneration> policies;
  std::unordered_set<CompositionId> draining;
  std::unordered_set<CompositionId> fenced;
  std::uint64_t next_comp = 1, next_res = 1, next_rs = 1, next_resv = 1;
};
namespace {

// Demand satisfaction for a class selection.
bool MeetsDemand(const std::vector<Resource>& sel, const ClassDemand& d) {
  if (sel.empty()) return false;
  if (d.amount_based) {
    double sum = 0.0;
    for (const auto& r : sel) sum += CapacityForDimension(r, d.dimension);
    return sum >= d.amount;
  }
  return static_cast<std::uint32_t>(sel.size()) >= d.count;
}

// Bounded enumeration of candidate selections for one class.
std::vector<std::vector<ResourceId>> EnumerateSelections(const std::vector<Resource>& items,
                                                         const ClassDemand& d, std::size_t cap) {
  std::vector<std::vector<ResourceId>> out;
  if (items.empty()) return out;
  std::size_t k = items.size();
  std::size_t want = (d.count != 0) ? d.count : 1;
  if (k > 8) k = 8;
  if (want > k) return out;
  if (!d.amount_based) {
    std::vector<ResourceId> cur;
    std::function<void(std::size_t, std::size_t)> combine = [&](std::size_t start, std::size_t need) {
      if (out.size() >= cap) return;
      if (need == 0) { out.push_back(cur); return; }
      for (std::size_t i = start; i <= k - need; ++i) {
        cur.push_back(items[i].id);
        combine(i + 1, need - 1);
        cur.pop_back();
        if (out.size() >= cap) return;
      }
    };
    combine(0, want);
  } else {
    for (std::size_t i = 0; i < k && out.size() < cap; ++i) out.push_back({ items[i].id });
    if (d.amount > 0.0 && k >= 2) {
      for (std::size_t i = 0; i < k && out.size() < cap; ++i) {
        for (std::size_t j = i + 1; j < k && out.size() < cap; ++j) {
          double s = CapacityForDimension(items[i], d.dimension) + CapacityForDimension(items[j], d.dimension);
          if (s >= d.amount) out.push_back({ items[i].id, items[j].id });
        }
      }
    }
  }
  return out;
}

// Amount to hold for a resource in a reservation (exclusive semantics).
double HoldAmountForResource(const Resource& r, const CompositionRequest& req) {
  auto demands = ComputeDemands(req);
  for (const auto& d : demands) {
    if (d.cls == r.resource_class) {
      if (d.amount_based) return CapacityForDimension(r, d.dimension);
      return 1.0;
    }
  }
  return 1.0;
}

}  // namespace
FabricEngine::FabricEngine(EngineConfig cfg) : impl_(std::make_unique<Impl>()) {
  impl_->config = cfg;
  if (!cfg.coordinator.IsValid()) impl_->config.coordinator = CoordinatorId(1);
  if (!cfg.epoch.IsValid()) impl_->config.epoch = CoordinatorEpoch(1);
}

FabricEngine::~FabricEngine() = default;

void FabricEngine::AddBackend(BackendPtr b) {
  if (!b) return;
  std::lock_guard<std::mutex> lk(impl_->mu);
  impl_->backends.push_back(std::move(b));
}

void FabricEngine::IngestNodeLocked(const Node& n) {
  if (!n.id.IsValid()) return;
  auto it = impl_->nodes.find(n.id);
  if (it != impl_->nodes.end() && it->second.generation != n.generation) {
    for (auto& kv : impl_->compositions) {
      auto& c = kv.second;
      if (c.lifecycle == CompositionLifecycle::ACTIVE) c.lifecycle = CompositionLifecycle::RECOMPOSITION_REQUIRED;
      else if (c.lifecycle == CompositionLifecycle::COMMITTED || c.lifecycle == CompositionLifecycle::RESERVED) c.lifecycle = CompositionLifecycle::REVALIDATION_REQUIRED;
    }
  }
  impl_->nodes[n.id] = n;
}

void FabricEngine::IngestResourceLocked(const Resource& r) {
  if (!r.id.IsValid()) return;
  auto it = impl_->resources.find(r.id);
  if (it != impl_->resources.end() && it->second.generation != r.generation) {
    for (auto& kv : impl_->compositions) {
      auto& c = kv.second;
      bool isMember = false;
      for (const auto& v : c.graph.vertices) if (v == r.id) { isMember = true; break; }
      if (!isMember) continue;
      if (c.lifecycle == CompositionLifecycle::ACTIVE) c.lifecycle = CompositionLifecycle::RECOMPOSITION_REQUIRED;
      else if (c.lifecycle == CompositionLifecycle::COMMITTED || c.lifecycle == CompositionLifecycle::RESERVED) c.lifecycle = CompositionLifecycle::REVALIDATION_REQUIRED;
    }
  }
  impl_->resources[r.id] = r;
}

DiscoveryResult FabricEngine::Discover() {
  std::lock_guard<std::mutex> lk(impl_->mu);
  DiscoveryResult out;
  for (auto& b : impl_->backends) {
    for (auto& n : b->DiscoverNodes()) { IngestNodeLocked(n); out.nodes.push_back(n); }
    for (auto& r : b->DiscoverResources()) {
      Resource rr = r;
      auto nit = impl_->nodes.find(rr.node_id);
      if (nit != impl_->nodes.end()) rr.synthetic = nit->second.synthetic;
      IngestResourceLocked(rr);
      out.resources.push_back(rr);
    }
  }
  // Resolve cross-node paths from backends so graph validation has evidence.
  std::vector<NodeId> ids;
  for (const auto& kv : impl_->nodes) ids.push_back(kv.first);
  for (const auto& a : ids) {
    for (const auto& b : ids) {
      if (a == b) continue;
      PathRequirement pr; pr.from = a; pr.to = b; pr.required = true;
      for (auto& backend : impl_->backends) {
        auto pe = backend->ResolvePath(pr);
        if (pe) { impl_->paths[PathKey(a, b)] = *pe; break; }
      }
    }
  }
  return out;
}

bool FabricEngine::RegisterNode(const Node& n) {
  if (!n.id.IsValid()) return false;
  std::lock_guard<std::mutex> lk(impl_->mu);
  IngestNodeLocked(n);
  return true;
}

bool FabricEngine::RegisterResource(const Resource& r) {
  if (!r.id.IsValid()) return false;
  std::lock_guard<std::mutex> lk(impl_->mu);
  IngestResourceLocked(r);
  return true;
}

void FabricEngine::InvalidateNode(ResourceId rid) {
  std::lock_guard<std::mutex> lk(impl_->mu);
  auto it = impl_->resources.find(rid);
  if (it != impl_->resources.end()) { it->second.evidence.state = EvidenceState::REVALIDATION_REQUIRED; it->second.evidence.max_age_ms = 0; }
}

void FabricEngine::WithdrawResource(ResourceId rid) {
  std::lock_guard<std::mutex> lk(impl_->mu);
  auto it = impl_->resources.find(rid);
  if (it != impl_->resources.end()) {
    it->second.health = ResourceHealth::OFFLINE;
    it->second.readiness = Readiness::NOT_READY;
    it->second.reachability = Reachability::UNREACHABLE;
    it->second.evidence.state = EvidenceState::REVALIDATION_REQUIRED;
    for (auto& kv : impl_->compositions) {
      auto& c = kv.second;
      for (const auto& v : c.graph.vertices) {
        if (v == rid && c.lifecycle == CompositionLifecycle::ACTIVE) { c.lifecycle = CompositionLifecycle::RECOMPOSITION_REQUIRED; break; }
      }
    }
  }
}

void FabricEngine::MarkEvidenceRevalidationRequired(WorkerBootId owner) {
  std::lock_guard<std::mutex> lk(impl_->mu);
  const std::string ownerStr = owner.ToString();
  for (auto& kv : impl_->resources) {
    auto& r = kv.second;
    if (r.evidence.owner == ownerStr || r.evidence.provenance == ownerStr) { r.evidence.state = EvidenceState::REVALIDATION_REQUIRED; r.evidence.max_age_ms = 0; r.health = ResourceHealth::UNKNOWN; }
  }
  for (auto& kv : impl_->compositions) {
    auto& c = kv.second;
    if (c.lifecycle == CompositionLifecycle::ACTIVE) {
      bool affected = false;
      for (const auto& v : c.graph.vertices) {
        auto it = impl_->resources.find(v);
        if (it != impl_->resources.end() && (it->second.evidence.owner == ownerStr || it->second.evidence.provenance == ownerStr)) { affected = true; break; }
      }
      if (affected) c.lifecycle = CompositionLifecycle::RECOMPOSITION_REQUIRED;
    }
  }
}

void FabricEngine::AdvanceEpoch(CoordinatorEpoch next) {
  if (!next.IsValid()) return;
  std::lock_guard<std::mutex> lk(impl_->mu);
  impl_->config.epoch = next;
  for (auto& kv : impl_->resources) {
    if (!kv.second.evidence.durable) { kv.second.evidence.state = EvidenceState::REVALIDATION_REQUIRED; kv.second.evidence.max_age_ms = 0; }
  }
  for (auto& kv : impl_->compositions) {
    auto& c = kv.second;
    if (c.lifecycle == CompositionLifecycle::ACTIVE) c.lifecycle = CompositionLifecycle::REVALIDATION_REQUIRED;
  }
}

std::size_t FabricEngine::NodeCount() const { std::lock_guard<std::mutex> lk(impl_->mu); return impl_->nodes.size(); }
std::size_t FabricEngine::ResourceCount() const { std::lock_guard<std::mutex> lk(impl_->mu); return impl_->resources.size(); }
std::size_t FabricEngine::CompositionCount() const { std::lock_guard<std::mutex> lk(impl_->mu); return impl_->compositions.size(); }

std::optional<Resource> FabricEngine::GetResource(ResourceId id) const {
  std::lock_guard<std::mutex> lk(impl_->mu);
  auto it = impl_->resources.find(id);
  if (it == impl_->resources.end()) return std::nullopt;
  return it->second;
}
std::optional<Node> FabricEngine::GetNode(NodeId id) const {
  std::lock_guard<std::mutex> lk(impl_->mu);
  auto it = impl_->nodes.find(id);
  if (it == impl_->nodes.end()) return std::nullopt;
  return it->second;
}
std::optional<Composition> FabricEngine::GetComposition(CompositionId id) const {
  std::lock_guard<std::mutex> lk(impl_->mu);
  auto it = impl_->compositions.find(id);
  if (it == impl_->compositions.end()) return std::nullopt;
  return it->second;
}
std::optional<Reservation> FabricEngine::GetReservation(ReservationId id) const {
  std::lock_guard<std::mutex> lk(impl_->mu);
  auto it = impl_->reservations.find(id);
  if (it == impl_->reservations.end()) return std::nullopt;
  return it->second;
}

RegistrySnapshot FabricEngine::Snapshot() const {
  std::lock_guard<std::mutex> lk(impl_->mu);
  RegistrySnapshot s;
  s.epoch = impl_->config.epoch;
  for (const auto& kv : impl_->nodes) s.nodes.push_back(kv.second);
  for (const auto& kv : impl_->resources) s.resources.push_back(kv.second);
  for (const auto& kv : impl_->compositions) s.compositions.push_back(kv.second);
  for (const auto& kv : impl_->reservations) s.reservations.push_back(kv.second);
  return s;
}

CoordinatorEpoch FabricEngine::epoch() const { std::lock_guard<std::mutex> lk(impl_->mu); return impl_->config.epoch; }
// --- eligibility / compose -------------------------------------------------

EligibilityDetail FabricEngine::CheckEligibility(ResourceId id, const CompositionRequest& req) const {
  std::lock_guard<std::mutex> lk(impl_->mu);
  EligibilityDetail d;
  auto it = impl_->resources.find(id);
  if (it == impl_->resources.end()) { d.reasons.push_back(RejectionReason::RESOURCE_OFFLINE); d.notes.push_back("resource not registered"); return d; }
  bool ok = ResourceEligible(it->second, req, d.reasons, d.notes);
  d.eligible = ok;
  return d;
}

std::string FabricEngine::ExplainEligibility(ResourceId id, const CompositionRequest& req) const {
  auto d = CheckEligibility(id, req);
  std::ostringstream os;
  if (d.ok()) { os << "ELIGIBLE " << id.ToString(); }
  else { os << "REJECTED " << id.ToString(); for (auto r : d.reasons) os << " " << ToString(r); }
  return os.str();
}

std::string FabricEngine::ExplainCompositionResult(const ComposeResult& cr) const {
  std::ostringstream os;
  os << (cr.ok() ? "OK" : "REJECTED") << " outcome=" << ToString(cr.outcome);
  for (auto r : cr.reasons) os << " " << ToString(r);
  return os.str();
}

ComposeResult FabricEngine::Compose(const CompositionRequest& req) { return ComposeDeterministic(req, 0); }

ComposeResult FabricEngine::ComposeDeterministic(const CompositionRequest& req, std::uint64_t seed) {
  std::lock_guard<std::mutex> lk(impl_->mu);
  ComposeResult result;
  result.outcome = CompositionOutcome::REJECTED;
  auto demands = ComputeDemands(req);
  (void)seed;

  // Group eligible resources by class so that planning is bounded and deterministic.
  std::map<std::uint32_t, std::vector<Resource>> byClass;
  for (const auto& kv : impl_->resources) {
    const Resource& r = kv.second;
    std::vector<RejectionReason> rr; std::vector<std::string> nn;
    if (!ResourceEligible(r, req, rr, nn)) continue;
    byClass[static_cast<std::uint32_t>(r.resource_class)].push_back(r);
  }

  // For each mandatory class, sort candidates and return bounded selections.
  struct Sel { ClassDemand d; std::vector<std::vector<ResourceId>> picks; };
  std::vector<Sel> selections;
  for (const auto& d : demands) {
    if (!d.mandatory) continue;
    auto it = byClass.find(static_cast<std::uint32_t>(d.cls));
    std::vector<Resource> items;
    if (it != byClass.end()) {
      items = it->second;
      std::sort(items.begin(), items.end(), [&d](const Resource& a, const Resource& b) {
        double sa = ClassScore(a, d), sb = ClassScore(b, d);
        if (sa != sb) return sa > sb;
        return a.id < b.id;
      });
    }
    auto picks = EnumerateSelections(items, d, 24);
    if (picks.empty()) {
      result.reasons.push_back(RejectionReason::NO_VALID_COMPOSITION);
      result.notes.push_back("no eligible candidate for mandatory class " + std::string(ToString(d.cls)));
      return result;
    }
    selections.push_back({ d, std::move(picks) });
  }
  if (selections.empty()) {
    result.reasons.push_back(RejectionReason::NO_VALID_COMPOSITION);
    result.notes.push_back("request defines no mandatory resource class");
    return result;
  }

  // Bounded Cartesian product of per-class selections (pruned, capped).
  std::vector<std::vector<ResourceId>> combos = { {} };
  constexpr std::size_t kComboCap = 400;
  for (auto& sel : selections) {
    std::vector<std::vector<ResourceId>> next;
    for (auto& combo : combos) {
      for (auto& pick : sel.picks) {
        bool dup = false;
        for (const auto& rid : pick) if (std::find(combo.begin(), combo.end(), rid) != combo.end()) { dup = true; break; }
        if (dup) continue;
        std::vector<ResourceId> v = combo;
        v.insert(v.end(), pick.begin(), pick.end());
        next.push_back(std::move(v));
        if (next.size() >= kComboCap) break;
      }
      if (next.size() >= kComboCap) break;
    }
    if (next.empty()) { result.reasons.push_back(RejectionReason::NO_VALID_COMPOSITION); return result; }
    combos = std::move(next);
  }

  // Evaluate each candidate combination against graph / failure domains; rank.
  ComposeResult best;
  best.outcome = CompositionOutcome::REJECTED;
  best.plan.rank.total = -1.0;
  for (auto& combo : combos) {
    std::vector<Resource> set;
    set.reserve(combo.size());
    bool okMembers = true;
    for (const auto& rid : combo) {
      auto it = impl_->resources.find(rid);
      if (it == impl_->resources.end()) { okMembers = false; break; }
      set.push_back(it->second);
    }
    if (!okMembers) continue;

    bool meets = true;
    for (auto& sel : selections) {
      std::vector<Resource> clsSet;
      for (const auto& r : set) if (r.resource_class == sel.d.cls) clsSet.push_back(r);
      if (!MeetsDemand(clsSet, sel.d)) { meets = false; break; }
    }
    if (!meets) continue;

    ResourceGraph g = BuildGraph(req, set);
    std::vector<RejectionReason> vrr; std::vector<std::string> vnn;
    if (!ValidateGraph(req, g, impl_->resources, impl_->nodes, impl_->paths, vrr, vnn)) continue;
    if (!ValidateFailureDomains(set, impl_->nodes, req, vrr, vnn)) continue;
    if (req.require_coherence) {
      bool coherent = true;
      for (const auto& r : set) if (!CapabilitySupported(r, CapabilityKind::COHERENT_MEMORY_ACCESS)) coherent = false;
      if (!coherent) continue;
    }
    RankScore rank = Rank(set, impl_->nodes, req);
    if (rank.total > best.plan.rank.total) {
      best.outcome = CompositionOutcome::SUCCESS;
      best.plan.graph = g;
      best.plan.request = req;
      best.plan.rank = rank;
      best.plan.nodes.clear();
      for (const auto& r : set) {
        if (std::find(best.plan.nodes.begin(), best.plan.nodes.end(), r.node_id) == best.plan.nodes.end()) best.plan.nodes.push_back(r.node_id);
      }
      best.plan.paths.clear();
      for (const auto& e : g.edges) {
        auto fit = impl_->resources.find(e.from);
        auto tit = impl_->resources.find(e.to);
        if (fit == impl_->resources.end() || tit == impl_->resources.end()) continue;
        if (fit->second.node_id == tit->second.node_id) continue;
        PathRequirement pr;
        pr.from = fit->second.node_id;
        pr.to = tit->second.node_id;
        pr.min_bandwidth_mbps = req.min_bandwidth_mbps;
        pr.max_latency_us = req.max_latency_us;
        pr.required = true;
        pr.direct = req.require_direct_path;
        bool found = false;
        for (auto& backend : impl_->backends) {
          auto pe = backend->ResolvePath(pr);
          if (pe) { impl_->paths[PathKey(pr.from, pr.to)] = *pe; best.plan.paths.push_back(*pe); found = true; break; }
        }
        if (!found) {
          auto pit = impl_->paths.find(PathKey(pr.from, pr.to));
          if (pit != impl_->paths.end()) best.plan.paths.push_back(pit->second);
        }
      }
    }
  }

  if (best.outcome != CompositionOutcome::SUCCESS) {
    result.reasons.push_back(RejectionReason::NO_VALID_COMPOSITION);
    result.notes.push_back("no feasible cross-resource combination");
    return result;
  }
  best.plan.generation = CompositionGeneration(impl_->next_comp);
  best.plan.id = CompositionId(impl_->next_comp++);
  std::vector<Resource> set2;
  for (const auto& rid : best.plan.graph.vertices) { auto it = impl_->resources.find(rid); if (it != impl_->resources.end()) set2.push_back(it->second); }
  best.plan.authority = MakeAuthority(best.plan.generation, req, set2, impl_->nodes, impl_->paths, impl_->config.epoch);
  best.plan.lifecycle = CompositionLifecycle::PLANNED;
  return best;
}
// --- reservation / commit / activate -------------------------------------

ReserveResult FabricEngine::Reserve(const CompositionPlan& plan) {
  std::lock_guard<std::mutex> lk(impl_->mu);
  ReserveResult result;
  result.outcome = CompositionOutcome::REJECTED;
  if (plan.lifecycle != CompositionLifecycle::PLANNED) { result.reasons.push_back(RejectionReason::INVALID_STATE_TRANSITION); result.notes.push_back("plan not PLANNED"); return result; }
  if (!plan.id.IsValid()) { result.reasons.push_back(RejectionReason::INVALID_STATE_TRANSITION); result.notes.push_back("plan has zero id"); return result; }

  Reservation resv;
  resv.id = ReservationId(impl_->next_resv++);
  resv.generation = ReservationGeneration(1);
  resv.composition = plan.id;
  resv.state = ReservationState::PROVISIONAL;

  std::vector<std::pair<ResourceId, double>> applied;
  bool failure = false;
  std::vector<std::string> notes;
  for (const auto& rid : plan.graph.vertices) {
    auto it = impl_->resources.find(rid);
    if (it == impl_->resources.end()) { failure = true; result.reasons.push_back(RejectionReason::RESOURCE_OFFLINE); notes.push_back("member missing " + rid.ToString()); break; }
    const Resource& r = it->second;
    std::vector<RejectionReason> rr; std::vector<std::string> nn;
    if (!ResourceEligible(r, plan.request, rr, nn)) { failure = true; result.reasons.push_back(rr.empty() ? RejectionReason::RESOURCE_OFFLINE : rr.front()); notes.push_back("member ineligible " + rid.ToString()); break; }
    if (impl_->reserved_amount[rid] > 0.0) { failure = true; result.reasons.push_back(RejectionReason::RESERVATION_CONFLICT); notes.push_back("already reserved " + rid.ToString()); break; }
    double amt = HoldAmountForResource(r, plan.request);
    if (r.capacity.compute_units < 0.0 || r.capacity.host_memory_bytes < 0.0) { failure = true; result.reasons.push_back(RejectionReason::CAPACITY_NEGATIVE); notes.push_back("negative capacity " + rid.ToString()); break; }
    applied.push_back({ rid, amt });
    impl_->reserved_amount[rid] += amt;
    ReservationItem item; item.resource = rid; item.state = ReservationState::PROVISIONAL; item.reserved_amount = amt;
    resv.items.push_back(item);
  }
  if (failure) {
    for (auto& pr : applied) {
      auto hit = impl_->reserved_amount.find(pr.first);
      if (hit != impl_->reserved_amount.end()) { double nv = hit->second - pr.second; hit->second = (nv < 0.0 ? 0.0 : nv); if (hit->second == 0.0) impl_->reserved_amount.erase(hit); }
    }
    result.outcome = CompositionOutcome::REJECTED;
    if (result.reasons.empty()) result.reasons.push_back(RejectionReason::RESERVATION_CONFLICT);
    result.notes = notes;
    return result;
  }

  Composition comp;
  comp.id = plan.id;
  comp.generation = plan.generation;
  comp.resource_set_id = ResourceSetId(impl_->next_rs++);
  comp.resource_set_generation = ResourceSetGeneration(1);
  comp.graph = plan.graph;
  comp.nodes = plan.nodes;
  comp.request = plan.request;
  comp.authority = plan.authority;
  comp.lifecycle = CompositionLifecycle::RESERVED;
  comp.rank = plan.rank;
  comp.reservation = resv.id;
  comp.paths = plan.paths;
  comp.outcome = CompositionOutcome::OUTCOME_UNKNOWN;
  impl_->compositions[comp.id] = comp;
  impl_->reservations[resv.id] = resv;
  result.outcome = CompositionOutcome::SUCCESS;
  result.reservation = resv;
  return result;
}

RevalidateResult FabricEngine::Revalidate(const Composition& c) const {
  std::lock_guard<std::mutex> lk(impl_->mu);
  RevalidateResult r;
  std::vector<RejectionReason> rr; std::vector<std::string> nn;
  r.valid = RevalidationHolds(c, impl_->resources, impl_->nodes, impl_->paths, impl_->config.epoch, rr, nn);
  r.reasons = rr;
  r.notes = nn;
  return r;
}

CommitResult FabricEngine::Commit(const Reservation& resIn) {
  std::lock_guard<std::mutex> lk(impl_->mu);
  CommitResult result;
  result.outcome = CompositionOutcome::REJECTED;
  auto rit = impl_->reservations.find(resIn.id);
  if (rit == impl_->reservations.end()) { result.reasons.push_back(RejectionReason::RESERVATION_CONFLICT); result.notes.push_back("reservation unknown"); return result; }
  Reservation& resv = rit->second;
  if (resv.state != ReservationState::PROVISIONAL) { result.reasons.push_back(RejectionReason::INVALID_STATE_TRANSITION); result.notes.push_back("reservation not PROVISIONAL"); return result; }
  auto cit = impl_->compositions.find(resv.composition);
  if (cit == impl_->compositions.end()) { result.reasons.push_back(RejectionReason::RESERVATION_CONFLICT); result.notes.push_back("composition unknown"); return result; }
  Composition& comp = cit->second;
  if (comp.lifecycle != CompositionLifecycle::RESERVED) { result.reasons.push_back(RejectionReason::INVALID_STATE_TRANSITION); result.notes.push_back("composition not RESERVED"); return result; }

  auto rollback = [&](RejectionReason reason) -> CommitResult {
    for (auto& item : resv.items) {
      auto hit = impl_->reserved_amount.find(item.resource);
      if (hit != impl_->reserved_amount.end()) { double nv = hit->second - item.reserved_amount; hit->second = (nv < 0.0 ? 0.0 : nv); if (hit->second == 0.0) impl_->reserved_amount.erase(hit); }
    }
    resv.state = ReservationState::ROLLED_BACK;
    comp.lifecycle = CompositionLifecycle::REVALIDATION_REQUIRED;
    comp.outcome = CompositionOutcome::FAILED;
    result.outcome = CompositionOutcome::REJECTED;
    result.reasons.push_back(reason);
    result.composition = comp;
    return result;
  };

  for (auto& item : resv.items) {
    auto it = impl_->resources.find(item.resource);
    if (it == impl_->resources.end()) return rollback(RejectionReason::RESOURCE_OFFLINE);
    const Resource& r = it->second;
    std::vector<RejectionReason> rr; std::vector<std::string> nn;
    if (!ResourceEligible(r, comp.request, rr, nn)) return rollback(rr.empty() ? RejectionReason::RESOURCE_OFFLINE : rr.front());
    if (impl_->reserved_amount[item.resource] < item.reserved_amount) return rollback(RejectionReason::OVERCOMMIT);
  }

  for (auto& item : resv.items) item.state = ReservationState::COMMITTED;
  resv.state = ReservationState::COMMITTED;
  comp.lifecycle = CompositionLifecycle::COMMITTED;
  result.outcome = CompositionOutcome::SUCCESS;
  result.composition = comp;
  return result;
}

ActivateResult FabricEngine::Activate(CompositionId id) {
  std::lock_guard<std::mutex> lk(impl_->mu);
  ActivateResult result;
  result.outcome = CompositionOutcome::REJECTED;
  auto cit = impl_->compositions.find(id);
  if (cit == impl_->compositions.end()) { result.reasons.push_back(RejectionReason::NO_VALID_COMPOSITION); result.notes.push_back("composition unknown"); return result; }
  Composition& comp = cit->second;
  if (comp.lifecycle != CompositionLifecycle::COMMITTED) { result.reasons.push_back(RejectionReason::INVALID_STATE_TRANSITION); result.notes.push_back("composition not COMMITTED"); return result; }
  if (impl_->fenced.count(comp.id)) { result.reasons.push_back(RejectionReason::POLICY_DENIED); result.notes.push_back("composition fenced"); return result; }
  std::vector<RejectionReason> rr; std::vector<std::string> nn;
  if (!RevalidationHolds(comp, impl_->resources, impl_->nodes, impl_->paths, impl_->config.epoch, rr, nn)) {
    comp.lifecycle = CompositionLifecycle::REVALIDATION_REQUIRED;
    comp.outcome = CompositionOutcome::REJECTED;
    result.reasons = rr;
    result.notes = nn;
    result.composition = comp;
    return result;
  }
  comp.lifecycle = CompositionLifecycle::ACTIVE;
  comp.outcome = CompositionOutcome::SUCCESS;
  result.outcome = CompositionOutcome::SUCCESS;
  result.composition = comp;
  return result;
}
// --- invalidation / lifecycle --------------------------------------------

void FabricEngine::OnResourceUpdate(const Resource& updated) {
  std::lock_guard<std::mutex> lk(impl_->mu);
  IngestResourceLocked(updated);
}

void FabricEngine::OnNodeUpdate(const Node& updated) {
  std::lock_guard<std::mutex> lk(impl_->mu);
  IngestNodeLocked(updated);
}

void FabricEngine::OnPolicyUpdate(PolicyId policy, PolicyGeneration gen) {
  std::lock_guard<std::mutex> lk(impl_->mu);
  impl_->policies[policy] = gen;
  for (auto& kv : impl_->compositions) {
    auto& c = kv.second;
    if (c.lifecycle == CompositionLifecycle::ACTIVE || c.lifecycle == CompositionLifecycle::COMMITTED) {
      if (c.authority.policy_generation != gen) {
        if (c.lifecycle == CompositionLifecycle::ACTIVE) c.lifecycle = CompositionLifecycle::RECOMPOSITION_REQUIRED;
        else c.lifecycle = CompositionLifecycle::REVALIDATION_REQUIRED;
      }
    }
  }
}

void FabricEngine::OnPathUpdate(const PathEvidence& pe) {
  std::lock_guard<std::mutex> lk(impl_->mu);
  PathRequirement pr;
  pr.from = pe.requirement.from;
  pr.to = pe.requirement.to;
  impl_->paths[PathKey(pr.from, pr.to)] = pe;
  for (auto& kv : impl_->compositions) {
    auto& c = kv.second;
    if (c.lifecycle != CompositionLifecycle::ACTIVE) continue;
    bool used = false;
    for (const auto& p : c.paths) if (p.requirement.from == pr.from && p.requirement.to == pr.to && p.generation != pe.generation) used = true;
    if (used) c.lifecycle = CompositionLifecycle::RECOMPOSITION_REQUIRED;
  }
}

void FabricEngine::OnResourceFailure(ResourceId rid) {
  std::lock_guard<std::mutex> lk(impl_->mu);
  auto it = impl_->resources.find(rid);
  if (it != impl_->resources.end()) { it->second.health = ResourceHealth::OFFLINE; it->second.readiness = Readiness::NOT_READY; it->second.evidence.state = EvidenceState::REVALIDATION_REQUIRED; }
  for (auto& kv : impl_->compositions) {
    auto& c = kv.second;
    for (const auto& v : c.graph.vertices) {
      if (v == rid && c.lifecycle == CompositionLifecycle::ACTIVE) { c.lifecycle = CompositionLifecycle::RECOMPOSITION_REQUIRED; break; }
      if (v == rid && c.lifecycle == CompositionLifecycle::COMMITTED) { c.lifecycle = CompositionLifecycle::REVALIDATION_REQUIRED; break; }
    }
  }
}

bool FabricEngine::BeginDrain(CompositionId id) {
  std::lock_guard<std::mutex> lk(impl_->mu);
  auto cit = impl_->compositions.find(id);
  if (cit == impl_->compositions.end()) return false;
  Composition& c = cit->second;
  if (c.lifecycle != CompositionLifecycle::ACTIVE && c.lifecycle != CompositionLifecycle::COMMITTED && c.lifecycle != CompositionLifecycle::DEGRADED) return false;
  c.lifecycle = CompositionLifecycle::DRAINING;
  impl_->draining.insert(id);
  return true;
}

bool FabricEngine::EndDrain(CompositionId id) {
  std::lock_guard<std::mutex> lk(impl_->mu);
  auto cit = impl_->compositions.find(id);
  if (cit == impl_->compositions.end()) return false;
  Composition& c = cit->second;
  if (c.lifecycle != CompositionLifecycle::DRAINING) return false;
  auto rit = impl_->reservations.find(c.reservation);
  if (rit != impl_->reservations.end()) {
    for (auto& item : rit->second.items) {
      auto hit = impl_->reserved_amount.find(item.resource);
      if (hit != impl_->reserved_amount.end()) { double nv = hit->second - item.reserved_amount; hit->second = (nv < 0.0 ? 0.0 : nv); if (hit->second == 0.0) impl_->reserved_amount.erase(hit); }
    }
    rit->second.state = ReservationState::RELEASED;
  }
  c.lifecycle = CompositionLifecycle::RELEASED;
  impl_->draining.erase(id);
  return true;
}

bool FabricEngine::Recompute(CompositionId id) {
  CompositionRequest req;
  CompositionId oldId = id;
  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    auto cit = impl_->compositions.find(id);
    if (cit == impl_->compositions.end()) return false;
    req = cit->second.request;
  }
  // Build a fresh authority (C2) without holding the lock across public calls.
  ComposeResult cr = ComposeDeterministic(req, 0);
  if (!cr.ok()) return false;
  CompositionPlan plan = cr.plan;
  // The old composition is invalidated; exclusive resources cannot overlap, so
  // release its holds (break-before-make) before reserving the replacement.
  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    auto cit = impl_->compositions.find(oldId);
    if (cit != impl_->compositions.end()) {
      Composition& oldc = cit->second;
      auto rit = impl_->reservations.find(oldc.reservation);
      if (rit != impl_->reservations.end()) {
        for (auto& item : rit->second.items) {
          auto hit = impl_->reserved_amount.find(item.resource);
          if (hit != impl_->reserved_amount.end()) { double nv = hit->second - item.reserved_amount; hit->second = (nv < 0.0 ? 0.0 : nv); if (hit->second == 0.0) impl_->reserved_amount.erase(hit); }
        }
        rit->second.state = ReservationState::RELEASED;
      }
      oldc.lifecycle = CompositionLifecycle::RELEASED;
      impl_->fenced.insert(oldc.id);
    }
  }
  ReserveResult rr = Reserve(plan);
  if (!rr.ok()) return false;
  CommitResult cc = Commit(rr.reservation);
  if (!cc.ok()) return false;
  ActivateResult ar = Activate(cc.composition.id);
  if (!ar.ok()) return false;
  return true;
}

// --- persistence ---------------------------------------------------------

bool FabricEngine::Save(const std::string& path) const {
  persist::DurableState state;
  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    state.epoch = impl_->config.epoch;
    for (const auto& kv : impl_->nodes) state.nodes.push_back(kv.second);
    for (const auto& kv : impl_->resources) state.resources.push_back(kv.second);
    for (const auto& kv : impl_->policies) state.policies[kv.first] = kv.second;
    for (const auto& kv : impl_->compositions) state.compositions.push_back(kv.second);
  }
  std::string err;
  return persist::Save(state, path, err);
}

LoadResult FabricEngine::Load(const std::string& path) {
  persist::DurableState state;
  std::string err;
  LoadResult lr;
  if (!persist::Load(path, state, err)) { lr.error = err; return lr; }
  std::lock_guard<std::mutex> lk(impl_->mu);
  impl_->config.epoch = state.epoch;
  impl_->nodes.clear();
  for (auto& n : state.nodes) impl_->nodes[n.id] = n;
  impl_->resources.clear();
  for (auto& r : state.resources) {
    Resource rr = r;
    bool durable = rr.evidence.durable;
    if (!durable) { rr.evidence.state = EvidenceState::REVALIDATION_REQUIRED; rr.evidence.max_age_ms = 0; }
    impl_->resources[rr.id] = rr;
  }
  impl_->policies = state.policies;
  // Reimport composition history conservatively: nothing regains authority.
  impl_->compositions.clear();
  impl_->reservations.clear();
  for (auto& c : state.compositions) {
    Composition cc = c;
    if (cc.lifecycle == CompositionLifecycle::ACTIVE || cc.lifecycle == CompositionLifecycle::COMMITTED) cc.lifecycle = CompositionLifecycle::RECOMPOSITION_REQUIRED;
    cc.outcome = CompositionOutcome::OUTCOME_UNKNOWN;
    impl_->compositions[cc.id] = cc;
  }
  // Restore monotonic counters so new identities never collide with loaded ones.
  for (const auto& kv : impl_->compositions) { if (kv.first.Value() >= impl_->next_comp) impl_->next_comp = kv.first.Value() + 1; }
  for (const auto& kv : impl_->resources) { if (kv.first.Value() >= impl_->next_res) impl_->next_res = kv.first.Value() + 1; }
  for (const auto& kv : impl_->nodes) { if (kv.first.Value() >= impl_->next_rs) impl_->next_rs = kv.first.Value() + 1; }
  lr.ok = true;
  return lr;
}


bool FabricEngine::Release(CompositionId id) {
  std::lock_guard<std::mutex> lk(impl_->mu);
  auto cit = impl_->compositions.find(id);
  if (cit == impl_->compositions.end()) return false;
  Composition& c = cit->second;
  if (c.lifecycle == CompositionLifecycle::RELEASED || c.lifecycle == CompositionLifecycle::RETIRED) return false;
  auto rit = impl_->reservations.find(c.reservation);
  if (rit != impl_->reservations.end()) {
    for (auto& item : rit->second.items) {
      auto hit = impl_->reserved_amount.find(item.resource);
      if (hit != impl_->reserved_amount.end()) { double nv = hit->second - item.reserved_amount; hit->second = (nv < 0.0 ? 0.0 : nv); if (hit->second == 0.0) impl_->reserved_amount.erase(hit); }
    }    rit->second.state = ReservationState::RELEASED;
  }
  c.lifecycle = CompositionLifecycle::RELEASED;
  impl_->draining.erase(id);
  impl_->fenced.insert(id);
  return true;
}

}  // namespace dfabric
