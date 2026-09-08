// Disaggregation Fabric — strongly typed identities.
// Bound to <cstdint>, no external deps. Zero is the forbidden/invalid sentinel.
#pragma once
#include <cstdint>
#include <string>
#include <functional>
#include <type_traits>
#include <stdexcept>

namespace dfabric {

// Thrown when an identity/generation decode or uniqueness check fails.
class IdentityError : public std::runtime_error {
 public:
  explicit IdentityError(const std::string& m) : std::runtime_error(m) {}
};

// Strong integral identity bound to a tag so distinct identity kinds can never
// silently interconvert. Value 0 is reserved as invalid/zero.
template <typename Tag>
class StrongId {
 public:
  using value_type = std::uint64_t;
  constexpr StrongId() noexcept = default;
  constexpr explicit StrongId(std::uint64_t v) noexcept : value_(v) {}

  [[nodiscard]] constexpr value_type Value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool IsZero() const noexcept { return value_ == 0; }
  [[nodiscard]] constexpr bool IsValid() const noexcept { return value_ != 0; }

  // Rejects a zero/sentinel value where an identity is mandatory.
  [[nodiscard]] static StrongId Require(std::uint64_t v, const char* what) {
    if (v == 0) throw IdentityError(std::string("zero identity rejected: ") + what);
    return StrongId(v);
  }

  friend constexpr bool operator==(StrongId a, StrongId b) noexcept { return a.value_ == b.value_; }
  friend constexpr bool operator!=(StrongId a, StrongId b) noexcept { return a.value_ != b.value_; }
  friend constexpr bool operator<(StrongId a, StrongId b) noexcept { return a.value_ < b.value_; }
  friend constexpr bool operator<=(StrongId a, StrongId b) noexcept { return a.value_ <= b.value_; }
  friend constexpr bool operator>(StrongId a, StrongId b) noexcept { return a.value_ > b.value_; }

  [[nodiscard]] std::string ToString() const { return std::to_string(value_); }

 private:
  std::uint64_t value_ = 0;
};

// Generations are monotonic per identity kind; 0 is never a valid generation.
template <typename Tag>
class StrongGeneration {
 public:
  using value_type = std::uint64_t;
  constexpr StrongGeneration() noexcept = default;
  constexpr explicit StrongGeneration(std::uint64_t v) noexcept : value_(v) {}

  [[nodiscard]] constexpr value_type Value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool IsZero() const noexcept { return value_ == 0; }
  [[nodiscard]] constexpr bool IsValid() const noexcept { return value_ != 0; }

  // Next generation; overflow wraps to 1 (0 reserved) and is the caller's
  // responsibility to detect via IsZero() on the returned value.
  [[nodiscard]] constexpr StrongGeneration Next() const noexcept {
    std::uint64_t v = value_ + 1;
    if (v == 0) v = 1;
    return StrongGeneration(v);
  }

  [[nodiscard]] static StrongGeneration Require(std::uint64_t v, const char* what) {
    if (v == 0) throw IdentityError(std::string("zero generation rejected: ") + what);
    return StrongGeneration(v);
  }

  friend constexpr bool operator==(StrongGeneration a, StrongGeneration b) noexcept { return a.value_ == b.value_; }
  friend constexpr bool operator!=(StrongGeneration a, StrongGeneration b) noexcept { return a.value_ != b.value_; }
  friend constexpr bool operator<(StrongGeneration a, StrongGeneration b) noexcept { return a.value_ < b.value_; }
  friend constexpr bool operator<=(StrongGeneration a, StrongGeneration b) noexcept { return a.value_ <= b.value_; }

  [[nodiscard]] std::string ToString() const { return std::to_string(value_); }

 private:
  std::uint64_t value_ = 0;
};

// ---- identity tag declarations -------------------------------------------
struct ResourceIdTag {};
struct ResourceGenTag {};
struct ResourceSetIdTag {};
struct ResourceSetGenTag {};
struct CompositionIdTag {};
struct CompositionGenTag {};
struct NodeIdTag {};
struct NodeGenTag {};
struct AttachmentIdTag {};
struct AttachmentGenTag {};
struct CapabilityIdTag {};
struct CapabilityGenTag {};
struct CompatibilityIdTag {};
struct CompatibilityGenTag {};
struct PathEvidenceIdTag {};
struct PathGenTag {};
struct ReservationIdTag {};
struct ReservationGenTag {};
struct PolicyIdTag {};
struct PolicyGenTag {};
struct EvidenceIdTag {};
struct EvidenceGenTag {};
struct WorkerIdTag {};
struct WorkerBootIdTag {};
struct CoordinatorIdTag {};
struct CoordinatorEpochTag {};
struct FailureDomainIdTag {};
struct ConsumerIdTag {};
struct ConsumerGenTag {};

// ---- public aliases --------------------------------------------------------
using ResourceId = StrongId<ResourceIdTag>;
using ResourceGeneration = StrongGeneration<ResourceGenTag>;
using ResourceSetId = StrongId<ResourceSetIdTag>;
using ResourceSetGeneration = StrongGeneration<ResourceSetGenTag>;
using CompositionId = StrongId<CompositionIdTag>;
using CompositionGeneration = StrongGeneration<CompositionGenTag>;
using NodeId = StrongId<NodeIdTag>;
using NodeGeneration = StrongGeneration<NodeGenTag>;
using AttachmentId = StrongId<AttachmentIdTag>;
using AttachmentGeneration = StrongGeneration<AttachmentGenTag>;
using CapabilityId = StrongId<CapabilityIdTag>;
using CapabilityGeneration = StrongGeneration<CapabilityGenTag>;
using CompatibilityId = StrongId<CompatibilityIdTag>;
using CompatibilityGeneration = StrongGeneration<CompatibilityGenTag>;
using PathEvidenceId = StrongId<PathEvidenceIdTag>;
using PathGeneration = StrongGeneration<PathGenTag>;
using ReservationId = StrongId<ReservationIdTag>;
using ReservationGeneration = StrongGeneration<ReservationGenTag>;
using PolicyId = StrongId<PolicyIdTag>;
using PolicyGeneration = StrongGeneration<PolicyGenTag>;
using EvidenceId = StrongId<EvidenceIdTag>;
using EvidenceGeneration = StrongGeneration<EvidenceGenTag>;
using WorkerId = StrongId<WorkerIdTag>;
using WorkerBootId = StrongId<WorkerBootIdTag>;
using CoordinatorId = StrongId<CoordinatorIdTag>;
using CoordinatorEpoch = StrongGeneration<CoordinatorEpochTag>;
using FailureDomainId = StrongId<FailureDomainIdTag>;
using ConsumerId = StrongId<ConsumerIdTag>;
using ConsumerGeneration = StrongGeneration<ConsumerGenTag>;

}  // namespace dfabric

// std::hash specializations for the id/generation types.
#define DFABRIC_DEFINE_HASH(TYPE)                                                        \
  template <> struct std::hash<::dfabric::TYPE> {                                         \
    std::size_t operator()(::dfabric::TYPE const& k) const noexcept {                     \
      return std::hash<std::uint64_t>{}(k.Value());                                       \
    }                                                                                     \
  }

namespace dfabric {
// (hash specializations must be in std; see below)
}
DFABRIC_DEFINE_HASH(ResourceId);
DFABRIC_DEFINE_HASH(ResourceGeneration);
DFABRIC_DEFINE_HASH(ResourceSetId);
DFABRIC_DEFINE_HASH(ResourceSetGeneration);
DFABRIC_DEFINE_HASH(CompositionId);
DFABRIC_DEFINE_HASH(CompositionGeneration);
DFABRIC_DEFINE_HASH(NodeId);
DFABRIC_DEFINE_HASH(NodeGeneration);
DFABRIC_DEFINE_HASH(AttachmentId);
DFABRIC_DEFINE_HASH(AttachmentGeneration);
DFABRIC_DEFINE_HASH(CapabilityId);
DFABRIC_DEFINE_HASH(CapabilityGeneration);
DFABRIC_DEFINE_HASH(CompatibilityId);
DFABRIC_DEFINE_HASH(CompatibilityGeneration);
DFABRIC_DEFINE_HASH(PathEvidenceId);
DFABRIC_DEFINE_HASH(PathGeneration);
DFABRIC_DEFINE_HASH(ReservationId);
DFABRIC_DEFINE_HASH(ReservationGeneration);
DFABRIC_DEFINE_HASH(PolicyId);
DFABRIC_DEFINE_HASH(PolicyGeneration);
DFABRIC_DEFINE_HASH(EvidenceId);
DFABRIC_DEFINE_HASH(EvidenceGeneration);
DFABRIC_DEFINE_HASH(WorkerId);
DFABRIC_DEFINE_HASH(WorkerBootId);
DFABRIC_DEFINE_HASH(CoordinatorId);
DFABRIC_DEFINE_HASH(CoordinatorEpoch);
DFABRIC_DEFINE_HASH(FailureDomainId);
DFABRIC_DEFINE_HASH(ConsumerId);
DFABRIC_DEFINE_HASH(ConsumerGeneration);

#undef DFABRIC_DEFINE_HASH
