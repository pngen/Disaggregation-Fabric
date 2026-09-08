// Disaggregation Fabric — canonical enumerations.
// Every enum carries a stable integral value used across persistence/protocol,
// plus a string form for inspection/explanation. Values are part of the wire
// and on-disk format: do not renumber existing values.
#pragma once
#include <string>
#include <cstdint>
#include <array>

namespace dfabric {

enum class ResourceClass : std::uint32_t {
  COMPUTE = 1,
  ACCELERATOR = 2,
  HOST_MEMORY = 3,
  EXPANDED_MEMORY = 4,
  STORAGE = 5,
  NETWORK_ENDPOINT = 6,
  FABRIC_ENDPOINT = 7,
  DPU = 8,
  NIC = 9,
  SERVICE_ENDPOINT = 10,
  CONTROL_PROCESS = 11,
  OTHER_REGISTERED = 12,
  UNKNOWN = 0,
};

enum class CapabilityKind : std::uint32_t {
  CUDA_EXECUTION = 1,
  ROCM_EXECUTION = 2,
  CPU_EXECUTION = 3,
  HOST_MEMORY_ACCESS = 4,
  EXPANDED_MEMORY_ACCESS = 5,
  PERSISTENT_STORAGE = 6,
  DIRECT_NETWORK_ACCESS = 7,
  RDMA_CAPABLE = 8,
  GPUDIRECT_CAPABLE = 9,
  COHERENT_MEMORY_ACCESS = 10,
  NON_COHERENT_MEMORY_ACCESS = 11,
  SPECIFIC_ISA = 12,
  ARCH_GENERATION = 13,
  DATA_FORMAT_COMPAT = 14,
  PAGE_SIZE_SUPPORT = 15,
  ADDRESS_WIDTH = 16,
  DMA_CAPABLE = 17,
  STORAGE_PROTOCOL = 18,
  NETWORK_PROTOCOL = 19,
  COMPUTE_PRECISION = 20,
  ATOMIC_SEMANTICS = 21,
  RETRY_CAPABLE = 22,
  DRAIN_SUPPORT = 23,
  UNKNOWN = 0,
};

enum class CapabilityState : std::uint32_t {
  SUPPORTED = 1,
  UNSUPPORTED = 2,
  UNKNOWN = 3,
  REVALIDATION_REQUIRED = 4,
};

enum class ResourceHealth : std::uint32_t {
  HEALTHY = 1,
  DEGRADED = 2,
  OFFLINE = 3,
  UNKNOWN = 0,
};

enum class Readiness : std::uint32_t {
  READY = 1,
  NOT_READY = 2,
  UNKNOWN = 0,
};

enum class Reachability : std::uint32_t {
  REACHABLE = 1,
  UNREACHABLE = 2,
  UNKNOWN = 0,
};

enum class EvidenceState : std::uint32_t {
  FRESH = 1,
  STALE = 2,
  REVALIDATION_REQUIRED = 3,
  PROVENANCE_UNKNOWN = 4,
};

enum class EvidenceSource : std::uint32_t {
  STATIC = 1,
  WORKER = 2,
  COORDINATOR = 3,
  BACKEND = 4,
  DIRECT = 5,
  UNKNOWN = 0,
};

enum class NodeLifecycle : std::uint32_t {
  DISCOVERED = 1,
  VALIDATED = 2,
  REVALIDATION_REQUIRED = 3,
  RETIRED = 4,
};

enum class FailureDomainType : std::uint32_t {
  HOST = 1,
  NUMA_NODE = 2,
  PCI_ROOT = 3,
  RACK = 4,
  POWER_DOMAIN = 5,
  NETWORK_DOMAIN = 6,
  STORAGE_DOMAIN = 7,
  FABRIC_DOMAIN = 8,
  CUSTOM = 9,
  UNKNOWN = 0,
};

enum class BackendKind : std::uint32_t {
  REAL = 1,
  SYNTHETIC = 2,
  UNSUPPORTED = 3,
  UNKNOWN = 0,
};

enum class CompositionLifecycle : std::uint32_t {
  REQUESTED = 1,
  DISCOVERING = 2,
  EVALUATED = 3,
  PLANNED = 4,
  RESERVING = 5,
  RESERVED = 6,
  REVALIDATING = 7,
  COMMITTED = 8,
  ACTIVE = 9,
  DEGRADED = 10,
  DRAINING = 11,
  RECOMPOSITION_REQUIRED = 12,
  REVALIDATION_REQUIRED = 13,
  FENCED = 14,
  FAILED = 15,
  RELEASED = 16,
  RETIRED = 17,
};

enum class RejectionReason : std::uint32_t {
  NONE = 0,
  RESOURCE_OFFLINE = 1,
  RESOURCE_DEGRADED = 2,
  RESOURCE_UNREACHABLE = 3,
  RESOURCE_REVALIDATION_REQUIRED = 4,
  CAPABILITY_UNSUPPORTED = 5,
  CAPABILITY_UNKNOWN = 6,
  COMPATIBILITY_MISMATCH = 7,
  INSUFFICIENT_CAPACITY = 8,
  PATH_UNAVAILABLE = 9,
  PATH_STALE = 10,
  BANDWIDTH_INSUFFICIENT = 11,
  LATENCY_LIMIT_EXCEEDED = 12,
  FAILURE_DOMAIN_CONFLICT = 13,
  COLLOCATION_REQUIRED = 14,
  ANTI_COLLOCATION_VIOLATED = 15,
  POLICY_DENIED = 16,
  WRONG_RESOURCE_GENERATION = 17,
  WRONG_NODE_GENERATION = 18,
  WRONG_WORKER_BOOT = 19,
  WRONG_EPOCH = 20,
  STALE_EVIDENCE = 21,
  NO_VALID_COMPOSITION = 22,
  DUPLICATE_RESERVATION = 23,
  RESERVATION_CONFLICT = 24,
  OVERCOMMIT = 25,
  DUPLICATE_RELEASE = 26,
  STALE_RELEASE = 27,
  RESOURCE_WITHDRAWN = 28,
  RESOURCE_DRAINING = 29,
  PATH_EVIDENCE_STALE = 30,
  UNVERIFIED_PATH = 31,
  CAPACITY_NEGATIVE = 32,
  INVALID_STATE_TRANSITION = 33,
  UNKNOWN = 0xFFFFFFFFu,
};

enum class CompositionOutcome : std::uint32_t {
  SUCCESS = 1,
  REJECTED = 2,
  PARTIAL = 3,
  ABORTED = 4,
  OUTCOME_UNKNOWN = 5,
  FAILED = 6,
};

enum class ReservationState : std::uint32_t {
  NONE = 0,
  PROVISIONAL = 1,
  COMMITTED = 2,
  RELEASED = 3,
  ROLLED_BACK = 4,
};

enum class DegradeMode : std::uint32_t {
  NONE = 0,
  DEGRADED_OK = 1,
  DEGRADED_NONE = 2,
};

enum class ProtocolContext : std::uint32_t {
  DEFAULT = 0,  // reserved
};

// ---- string / enum conversion helpers -------------------------------------
// These are compile-time tables so that decoding a value that is not present in
// its canonical table fails closed instead of admitting an unknown value.
constexpr const char* ToString(ResourceClass v) {
  switch (v) {
    case ResourceClass::COMPUTE: return "COMPUTE";
    case ResourceClass::ACCELERATOR: return "ACCELERATOR";
    case ResourceClass::HOST_MEMORY: return "HOST_MEMORY";
    case ResourceClass::EXPANDED_MEMORY: return "EXPANDED_MEMORY";
    case ResourceClass::STORAGE: return "STORAGE";
    case ResourceClass::NETWORK_ENDPOINT: return "NETWORK_ENDPOINT";
    case ResourceClass::FABRIC_ENDPOINT: return "FABRIC_ENDPOINT";
    case ResourceClass::DPU: return "DPU";
    case ResourceClass::NIC: return "NIC";
    case ResourceClass::SERVICE_ENDPOINT: return "SERVICE_ENDPOINT";
    case ResourceClass::CONTROL_PROCESS: return "CONTROL_PROCESS";
    case ResourceClass::OTHER_REGISTERED: return "OTHER_REGISTERED";
    case ResourceClass::UNKNOWN: return "UNKNOWN";
  }
  return "UNKNOWN";
}
constexpr const char* ToString(CapabilityKind v) {
  switch (v) {
    case CapabilityKind::CUDA_EXECUTION: return "CUDA_EXECUTION";
    case CapabilityKind::ROCM_EXECUTION: return "ROCM_EXECUTION";
    case CapabilityKind::CPU_EXECUTION: return "CPU_EXECUTION";
    case CapabilityKind::HOST_MEMORY_ACCESS: return "HOST_MEMORY_ACCESS";
    case CapabilityKind::EXPANDED_MEMORY_ACCESS: return "EXPANDED_MEMORY_ACCESS";
    case CapabilityKind::PERSISTENT_STORAGE: return "PERSISTENT_STORAGE";
    case CapabilityKind::DIRECT_NETWORK_ACCESS: return "DIRECT_NETWORK_ACCESS";
    case CapabilityKind::RDMA_CAPABLE: return "RDMA_CAPABLE";
    case CapabilityKind::GPUDIRECT_CAPABLE: return "GPUDIRECT_CAPABLE";
    case CapabilityKind::COHERENT_MEMORY_ACCESS: return "COHERENT_MEMORY_ACCESS";
    case CapabilityKind::NON_COHERENT_MEMORY_ACCESS: return "NON_COHERENT_MEMORY_ACCESS";
    case CapabilityKind::SPECIFIC_ISA: return "SPECIFIC_ISA";
    case CapabilityKind::ARCH_GENERATION: return "ARCH_GENERATION";
    case CapabilityKind::DATA_FORMAT_COMPAT: return "DATA_FORMAT_COMPAT";
    case CapabilityKind::PAGE_SIZE_SUPPORT: return "PAGE_SIZE_SUPPORT";
    case CapabilityKind::ADDRESS_WIDTH: return "ADDRESS_WIDTH";
    case CapabilityKind::DMA_CAPABLE: return "DMA_CAPABLE";
    case CapabilityKind::STORAGE_PROTOCOL: return "STORAGE_PROTOCOL";
    case CapabilityKind::NETWORK_PROTOCOL: return "NETWORK_PROTOCOL";
    case CapabilityKind::COMPUTE_PRECISION: return "COMPUTE_PRECISION";
    case CapabilityKind::ATOMIC_SEMANTICS: return "ATOMIC_SEMANTICS";
    case CapabilityKind::RETRY_CAPABLE: return "RETRY_CAPABLE";
    case CapabilityKind::DRAIN_SUPPORT: return "DRAIN_SUPPORT";
    case CapabilityKind::UNKNOWN: return "UNKNOWN";
  }
  return "UNKNOWN";
}
constexpr const char* ToString(CapabilityState v) {
  switch (v) {
    case CapabilityState::SUPPORTED: return "SUPPORTED";
    case CapabilityState::UNSUPPORTED: return "UNSUPPORTED";
    case CapabilityState::UNKNOWN: return "UNKNOWN";
    case CapabilityState::REVALIDATION_REQUIRED: return "REVALIDATION_REQUIRED";
  }
  return "UNKNOWN";
}
constexpr const char* ToString(ResourceHealth v) {
  switch (v) {
    case ResourceHealth::HEALTHY: return "HEALTHY";
    case ResourceHealth::DEGRADED: return "DEGRADED";
    case ResourceHealth::OFFLINE: return "OFFLINE";
    case ResourceHealth::UNKNOWN: return "UNKNOWN";
  }
  return "UNKNOWN";
}
constexpr const char* ToString(Readiness v) {
  switch (v) {
    case Readiness::READY: return "READY";
    case Readiness::NOT_READY: return "NOT_READY";
    case Readiness::UNKNOWN: return "UNKNOWN";
  }
  return "UNKNOWN";
}
constexpr const char* ToString(Reachability v) {
  switch (v) {
    case Reachability::REACHABLE: return "REACHABLE";
    case Reachability::UNREACHABLE: return "UNREACHABLE";
    case Reachability::UNKNOWN: return "UNKNOWN";
  }
  return "UNKNOWN";
}
constexpr const char* ToString(EvidenceState v) {
  switch (v) {
    case EvidenceState::FRESH: return "FRESH";
    case EvidenceState::STALE: return "STALE";
    case EvidenceState::REVALIDATION_REQUIRED: return "REVALIDATION_REQUIRED";
    case EvidenceState::PROVENANCE_UNKNOWN: return "PROVENANCE_UNKNOWN";
  }
  return "UNKNOWN";
}
constexpr const char* ToString(EvidenceSource v) {
  switch (v) {
    case EvidenceSource::STATIC: return "STATIC";
    case EvidenceSource::WORKER: return "WORKER";
    case EvidenceSource::COORDINATOR: return "COORDINATOR";
    case EvidenceSource::BACKEND: return "BACKEND";
    case EvidenceSource::DIRECT: return "DIRECT";
    case EvidenceSource::UNKNOWN: return "UNKNOWN";
  }
  return "UNKNOWN";
}
constexpr const char* ToString(NodeLifecycle v) {
  switch (v) {
    case NodeLifecycle::DISCOVERED: return "DISCOVERED";
    case NodeLifecycle::VALIDATED: return "VALIDATED";
    case NodeLifecycle::REVALIDATION_REQUIRED: return "REVALIDATION_REQUIRED";
    case NodeLifecycle::RETIRED: return "RETIRED";
  }
  return "UNKNOWN";
}
constexpr const char* ToString(FailureDomainType v) {
  switch (v) {
    case FailureDomainType::HOST: return "HOST";
    case FailureDomainType::NUMA_NODE: return "NUMA_NODE";
    case FailureDomainType::PCI_ROOT: return "PCI_ROOT";
    case FailureDomainType::RACK: return "RACK";
    case FailureDomainType::POWER_DOMAIN: return "POWER_DOMAIN";
    case FailureDomainType::NETWORK_DOMAIN: return "NETWORK_DOMAIN";
    case FailureDomainType::STORAGE_DOMAIN: return "STORAGE_DOMAIN";
    case FailureDomainType::FABRIC_DOMAIN: return "FABRIC_DOMAIN";
    case FailureDomainType::CUSTOM: return "CUSTOM";
    case FailureDomainType::UNKNOWN: return "UNKNOWN";
  }
  return "UNKNOWN";
}
constexpr const char* ToString(BackendKind v) {
  switch (v) {
    case BackendKind::REAL: return "REAL";
    case BackendKind::SYNTHETIC: return "SYNTHETIC";
    case BackendKind::UNSUPPORTED: return "UNSUPPORTED";
    case BackendKind::UNKNOWN: return "UNKNOWN";
  }
  return "UNKNOWN";
}
constexpr const char* ToString(CompositionLifecycle v) {
  switch (v) {
    case CompositionLifecycle::REQUESTED: return "REQUESTED";
    case CompositionLifecycle::DISCOVERING: return "DISCOVERING";
    case CompositionLifecycle::EVALUATED: return "EVALUATED";
    case CompositionLifecycle::PLANNED: return "PLANNED";
    case CompositionLifecycle::RESERVING: return "RESERVING";
    case CompositionLifecycle::RESERVED: return "RESERVED";
    case CompositionLifecycle::REVALIDATING: return "REVALIDATING";
    case CompositionLifecycle::COMMITTED: return "COMMITTED";
    case CompositionLifecycle::ACTIVE: return "ACTIVE";
    case CompositionLifecycle::DEGRADED: return "DEGRADED";
    case CompositionLifecycle::DRAINING: return "DRAINING";
    case CompositionLifecycle::RECOMPOSITION_REQUIRED: return "RECOMPOSITION_REQUIRED";
    case CompositionLifecycle::REVALIDATION_REQUIRED: return "REVALIDATION_REQUIRED";
    case CompositionLifecycle::FENCED: return "FENCED";
    case CompositionLifecycle::FAILED: return "FAILED";
    case CompositionLifecycle::RELEASED: return "RELEASED";
    case CompositionLifecycle::RETIRED: return "RETIRED";
  }
  return "UNKNOWN";
}
constexpr const char* ToString(RejectionReason v) {
  switch (v) {
    case RejectionReason::NONE: return "NONE";
    case RejectionReason::RESOURCE_OFFLINE: return "RESOURCE_OFFLINE";
    case RejectionReason::RESOURCE_DEGRADED: return "RESOURCE_DEGRADED";
    case RejectionReason::RESOURCE_UNREACHABLE: return "RESOURCE_UNREACHABLE";
    case RejectionReason::RESOURCE_REVALIDATION_REQUIRED: return "RESOURCE_REVALIDATION_REQUIRED";
    case RejectionReason::CAPABILITY_UNSUPPORTED: return "CAPABILITY_UNSUPPORTED";
    case RejectionReason::CAPABILITY_UNKNOWN: return "CAPABILITY_UNKNOWN";
    case RejectionReason::COMPATIBILITY_MISMATCH: return "COMPATIBILITY_MISMATCH";
    case RejectionReason::INSUFFICIENT_CAPACITY: return "INSUFFICIENT_CAPACITY";
    case RejectionReason::PATH_UNAVAILABLE: return "PATH_UNAVAILABLE";
    case RejectionReason::PATH_STALE: return "PATH_STALE";
    case RejectionReason::BANDWIDTH_INSUFFICIENT: return "BANDWIDTH_INSUFFICIENT";
    case RejectionReason::LATENCY_LIMIT_EXCEEDED: return "LATENCY_LIMIT_EXCEEDED";
    case RejectionReason::FAILURE_DOMAIN_CONFLICT: return "FAILURE_DOMAIN_CONFLICT";
    case RejectionReason::COLLOCATION_REQUIRED: return "COLLOCATION_REQUIRED";
    case RejectionReason::ANTI_COLLOCATION_VIOLATED: return "ANTI_COLLOCATION_VIOLATED";
    case RejectionReason::POLICY_DENIED: return "POLICY_DENIED";
    case RejectionReason::WRONG_RESOURCE_GENERATION: return "WRONG_RESOURCE_GENERATION";
    case RejectionReason::WRONG_NODE_GENERATION: return "WRONG_NODE_GENERATION";
    case RejectionReason::WRONG_WORKER_BOOT: return "WRONG_WORKER_BOOT";
    case RejectionReason::WRONG_EPOCH: return "WRONG_EPOCH";
    case RejectionReason::STALE_EVIDENCE: return "STALE_EVIDENCE";
    case RejectionReason::NO_VALID_COMPOSITION: return "NO_VALID_COMPOSITION";
    case RejectionReason::DUPLICATE_RESERVATION: return "DUPLICATE_RESERVATION";
    case RejectionReason::RESERVATION_CONFLICT: return "RESERVATION_CONFLICT";
    case RejectionReason::OVERCOMMIT: return "OVERCOMMIT";
    case RejectionReason::DUPLICATE_RELEASE: return "DUPLICATE_RELEASE";
    case RejectionReason::STALE_RELEASE: return "STALE_RELEASE";
    case RejectionReason::RESOURCE_WITHDRAWN: return "RESOURCE_WITHDRAWN";
    case RejectionReason::RESOURCE_DRAINING: return "RESOURCE_DRAINING";
    case RejectionReason::PATH_EVIDENCE_STALE: return "PATH_EVIDENCE_STALE";
    case RejectionReason::UNVERIFIED_PATH: return "UNVERIFIED_PATH";
    case RejectionReason::CAPACITY_NEGATIVE: return "CAPACITY_NEGATIVE";
    case RejectionReason::INVALID_STATE_TRANSITION: return "INVALID_STATE_TRANSITION";
    case RejectionReason::UNKNOWN: return "UNKNOWN";
  }
  return "UNKNOWN";
}
constexpr const char* ToString(CompositionOutcome v) {
  switch (v) {
    case CompositionOutcome::SUCCESS: return "SUCCESS";
    case CompositionOutcome::REJECTED: return "REJECTED";
    case CompositionOutcome::PARTIAL: return "PARTIAL";
    case CompositionOutcome::ABORTED: return "ABORTED";
    case CompositionOutcome::OUTCOME_UNKNOWN: return "OUTCOME_UNKNOWN";
    case CompositionOutcome::FAILED: return "FAILED";
  }
  return "UNKNOWN";
}
constexpr const char* ToString(ReservationState v) {
  switch (v) {
    case ReservationState::NONE: return "NONE";
    case ReservationState::PROVISIONAL: return "PROVISIONAL";
    case ReservationState::COMMITTED: return "COMMITTED";
    case ReservationState::RELEASED: return "RELEASED";
    case ReservationState::ROLLED_BACK: return "ROLLED_BACK";
  }
  return "UNKNOWN";
}

// Canonical bounds of each enum (max valid value). Decoders must reject values
// outside [1..Max] (or ==0 where 0 is a sentinel) and any value not in the table.
template <typename E>
struct EnumTraits;

#define DFABRIC_ENUM_TRAITS(ENUM, MAXVALUE)                                   \
  template <> struct EnumTraits<ENUM> {                                       \
    static constexpr ENUM Min() noexcept { return ENUM(1); }                  \
    static constexpr ENUM Max() noexcept { return ENUM(MAXVALUE); }           \
    static constexpr std::uint32_t MaxValue = MAXVALUE;                       \
  }

DFABRIC_ENUM_TRAITS(ResourceClass, 12);
DFABRIC_ENUM_TRAITS(CapabilityKind, 23);
DFABRIC_ENUM_TRAITS(CapabilityState, 4);
DFABRIC_ENUM_TRAITS(ResourceHealth, 3);
DFABRIC_ENUM_TRAITS(Readiness, 2);
DFABRIC_ENUM_TRAITS(Reachability, 2);
DFABRIC_ENUM_TRAITS(EvidenceState, 4);
DFABRIC_ENUM_TRAITS(EvidenceSource, 5);
DFABRIC_ENUM_TRAITS(NodeLifecycle, 4);
DFABRIC_ENUM_TRAITS(FailureDomainType, 9);
DFABRIC_ENUM_TRAITS(BackendKind, 3);
DFABRIC_ENUM_TRAITS(CompositionLifecycle, 17);
DFABRIC_ENUM_TRAITS(RejectionReason, 33);
DFABRIC_ENUM_TRAITS(CompositionOutcome, 6);
DFABRIC_ENUM_TRAITS(ReservationState, 4);

#undef DFABRIC_ENUM_TRAITS

}  // namespace dfabric
