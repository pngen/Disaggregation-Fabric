# Disaggregation Fabric

Disaggregation Fabric is a vendor-neutral C++20 control-plane runtime that
decides which physically separated infrastructure resources (compute, accelerator,
memory, storage, networking, services) may be composed into a single authoritative
execution resource set, and whether that composition remains authoritative.

## Core question

Which disaggregated resources may be composed into a valid execution set now,
across which physical nodes and paths, under what capability, locality,
reachability, capacity, compatibility, reservation, failure-domain, generation, and
authority constraints — and when must that composition be rejected, drained,
replaced, revalidated, or recomposed?

The runtime makes explicit the difference between: *resource exists*, *resource is
reachable*, *resource is current*, *resource is compatible*, *resource is
reservable*, *resource is composition-eligible*, *resource set is internally
compatible*, *required paths are available*, *resource set is committed*,
*composition is authoritative now*, and *execution may safely proceed*. These states
are not collapsed.

## Boundary

Disaggregation Fabric owns generic resource-composition control-plane semantics:
strongly typed identities, node/resource/generation models, composition graphs,
hard feasibility, deterministic planning and ranking, atomic all-or-nothing
reservation, guarded lifecycle, authority binding, fencing, failure-domain
constraints, partial-resource failure, recomposition, drain, persistence, and
inspection.

It does **not** own general workload scheduling, Resource Broker's global
arbitration, Memory Expansion Fabric's generic expanded-memory capacity semantics,
CXL discovery, Near-Memory Compute placement, Transfer Fabric byte movement,
Communication Planner path construction, Fabric Scheduler placement, GPU Memory
Service allocation, storage movement, RDMA/GPUDirect/NVLink/NVSwitch, NIC/DPU
execution, virtualization, Kubernetes-style orchestration, software-defined
networking, or switch configuration. It is not a scheduler, broker, Kubernetes
replacement, network controller, distributed OS, or virtualization layer.

- **Resource Broker** answers: *what capacity exists, who may reserve it, what is
  allocated, which claim is authoritative?* Disaggregation Fabric may use a narrow
  adapter to it for reservation evidence; the standalone core does not require it.
- **Communication Planner** determines *how data moves across topology*.
  Disaggregation Fabric may require valid paths between selected components and may
  consume path-feasibility evidence through an optional narrow adapter, but never
  invents connectivity.
- **Memory Expansion Fabric** governs expanded/pooled memory capacity.
  Disaggregation Fabric may compose compute on one node with expanded memory on
  another when the relevant memory and path evidence make it valid.
- **Near-Memory Compute** asks *should computation move toward data*; it may be a
  downstream consumer of a disaggregated resource composition.

## Resource model

Each resource carries a ResourceId, ResourceGeneration, ResourceClass, NodeId,
provider identity, capabilities, capacity, health, readiness, reachability,
locality, failure domain, compatibility identity, evidence state, lifecycle, and
authority owner. Resource classes include COMPUTE, ACCELERATOR, HOST_MEMORY,
EXPANDED_MEMORY, STORAGE, NETWORK_ENDPOINT, FABRIC_ENDPOINT, DPU, NIC,
SERVICE_ENDPOINT, CONTROL_PROCESS, OTHER_REGISTERED, and UNKNOWN. An enum presence
does not imply physical support.

## Node model

Nodes carry an identity, generation, hostname/label, provider/backend, resource
inventory, failure domain, locality, reachability, health, worker ownership,
evidence freshness, and lifecycle. Node presence does not authorize its resources;
a node restart produces fresh process/incarnation authority.

## Composition graph

A resource set is modeled as a graph: vertices are resources, edges are required
relationships (reachability, latency bound, bandwidth bound, compatibility,
protocol, locality, freshness, generation). A composition is valid only if every
mandatory vertex and edge constraint passes; individually valid resources do not
form a valid set if their relationships are invalid.

## Requirement model

A CompositionRequest expresses required compute units, accelerator count and
architecture, accelerator/host/expanded memory, storage, network endpoints,
minimum bandwidth, maximum latency, topology/locality, failure-domain constraints,
direct-path, staging, coherence, persistence, compatibility identity, isolation,
colocation/anti-colocation, mandatory vs optional classes, fallback policy, policy
generation, and consumer generation. Hard requirements remain hard.

## Eligibility and ranking

Hard eligibility is applied before ranking with typed rejection reasons
(RESOURCE_OFFLINE, CAPABILITY_UNSUPPORTED, PATH_UNAVAILABLE, WRONG_RESOURCE_GENERATION,
STALE_EVIDENCE, FAILURE_DOMAIN_CONFLICT, NO_VALID_COMPOSITION, etc.). A good score
never rescues an invalid composition. Planning uses staged, bounded filtering
(top-K candidates per class) to avoid Cartesian explosion. After feasibility,
compositions are ranked deterministically with named factors; deterministic
tie-breaking and insertion-order independence are tested.

## Authority

A composition is bound to a CoordinatorEpoch, WorkerBootIds, NodeGenerations,
ResourceGenerations, PathGenerations, CapabilityGenerations, CompatibilityGenerations,
PolicyGeneration, EvidenceGenerations, ReservationGenerations, and
CompositionGeneration. Pre-activation revalidation is mandatory. When a required
generation changes, the composition becomes REVALIDATION_REQUIRED, FENCED, INVALID,
or RECOMPOSITION_REQUIRED.

## Reservation, commit, activation

Multi-resource reservation is atomic (plan -> provisional holds -> verify all
members -> commit all -> activate). If any member fails, every provisional hold is
released and accounting returns exactly to baseline. No partial authoritative
composition survives. Lifecycle is guarded: REQUESTED, DISCOVERING, EVALUATED,
PLANNED, RESERVING, RESERVED, REVALIDATING, COMMITTED, ACTIVE, DEGRADED, DRAINING,
RECOMPOSITION_REQUIRED, REVALIDATION_REQUIRED, FENCED, FAILED, RELEASED, RETIRED.
Illegal transitions are rejected and tested.

## Recomposition and drain

Recomposition builds a fresh composition under a new CompositionGeneration; the
invalidated old composition is released/fenced before the replacement reserves
exclusive resources (break-before-make is used where exclusive resources cannot
safely overlap). Drain rejects forbidden new holds, preserves authorized work, and
closes only after accounting reaches zero.

## Failure domains

Failure domains are represented explicitly (HOST, NUMA_NODE, PCI_ROOT, RACK,
POWER_DOMAIN, NETWORK_DOMAIN, STORAGE_DOMAIN, FABRIC_DOMAIN, CUSTOM); synthetic
domains are labelled SYNTHETIC. Requirements may enforce colocate, spread, distinct
failure domains, or max-shared-risk.

## Persistence and protocol

Persistence is versioned and integrity-checked (magic, format version, bounded
counts/lengths, checksum, corruption/truncation/malformed-enum/invalid-ID rejection,
atomic replacement). Live worker authority is never persisted as current; after
restart dynamic evidence becomes REVALIDATION_REQUIRED and old ACTIVE compositions
do not regain authority.

The protocol uses framed TCP with version, canonical frame-type bounds (no stale
numeric maximum), correlation, bounded length, and a frame checksum. The decoder
handles partial reads; malformed/oversized/unknown frames and checksum failures are
rejected. The newest frame types are tested.

## Process model

A real distributed reference deployment is provided: a Coordinator process running
the FabricEngine and Worker processes publishing resource evidence over real framed
TCP. Independent OS processes are used, and worker/coordinator death is proven with
a real OS kill plus a retained and verified process handle.

## REAL / SYNTHETIC / UNSUPPORTED matrix

- **REAL**: single-host resource discovery (CPU/NUMA/host memory/storage/network on
  this machine) and real backend operations (host-memory allocate/write/verify/free,
  bounded temp-file storage write/read/verify/delete, real loopback TCP transfer),
  plus real CUDA execution on the NVIDIA GeForce RTX 5090 (cudaMalloc / H2D / kernel
  / sync / D2H / CPU parity / cudaFree) gated by an authoritative composition, with
  the device-memory baseline restored.
- **SYNTHETIC**: multi-node resource composition, cross-node relationship/path
  semantics, failure, node loss, recomposition, make-before-break (as release-then-
  rebuild on exclusive resources), reservation rollback, and stale replay. All
  multi-node topology is explicitly labelled SYNTHETIC.
- **UNSUPPORTED**: physical multi-node disaggregation on this single workstation;
  CXL memory, RDMA fabric, NVLink/NVSwitch, DPU/SmartNIC, and rack topology are not
  physically exercised and are not claimed.

## Build

Requires CMake 3.22+, a C++20 compiler (MSVC 19.44 or later), and on Windows the
MSVC toolchain. CUDA 12.9+ is needed only for the real CUDA proof, which is built in Release
configurations (the nvcc host compiler links the MSVC dynamic runtime, which is
validated in Release; Debug builds run the full C++ test suite without the CUDA
proof).

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build --output-on-failure

## Tests

Unit tests (identity, lifecycle, reservation rollback, generation invalidation,
stale rejection, deterministic ranking, failure domains), property tests (seeded,
success implies valid graph / failure leaves no leak), concurrency tests (exclusive
reservation conflict), adversarial tests (duplicate registration, double release,
commit rollback), protocol framing tests, persistence integrity tests, and the
multiprocess proofs: worker death, member-resource generation loss, coordinator
restart, and interrupted-transaction recovery.

## Examples

Examples live in the build output as test executables. Representative scenarios:
basic_composition, gpu_memory_storage_set, hard_constraint_rejection,
failure_domain_policy, reservation_rollback, resource_generation_change,
recomposition, make_before_break, synthetic_multinode (via the synthetic backend),
coordinator_recovery (via the restart and interrupted proofs), and
real_cuda_composition (real RTX 5090).

## CLI

A narrow inspection CLI is provided:

    build/dfabric_cli discover system|synthetic
    build/dfabric_cli nodes
    build/dfabric_cli resources
    build/dfabric_cli compose
    build/dfabric_cli explain
    build/dfabric_cli inspect-state
    build/dfabric_cli audit

Output distinguishes REAL / SYNTHETIC / UNSUPPORTED / UNKNOWN / REVALIDATION_REQUIRED
where applicable.

## Benchmark

A benchmark measures completed operations (resource ingestion, constraint
filtering, composition generation, ranking, reservation, commit, state snapshot,
lookup) across 100 / 1,000 / 10,000 resources. Planning uses bounded staged
filtering (top-K candidates per class), so scaling is near-linear and no Cartesian
explosion is observed at tested scale.

## Package and installed consumer

The repository provides an installable CMake package. Install to a prefix and use:

    find_package(DisaggregationFabric CONFIG REQUIRED)
    target_link_libraries(myapp PRIVATE DisaggregationFabric::core DisaggregationFabric::synthetic)

A downstream consumer outside the source tree builds against the installed package,
registers synthetic resources, composes a valid resource set, validates it, and
exits 0.

## Hardware validation

- Real host discovery: CPU, host memory, storage, and a network endpoint on this
  machine.
- Real CUDA: NVIDIA GeForce RTX 5090 (SM 12.0, ~32 GiB).
- Physical multi-node disaggregation: UNSUPPORTED on this single workstation.

## Limitations

Only a single physical workstation is available. Physical multi-node disaggregation,
CXL memory, RDMA fabric, GPUDirect, NVLink/NVSwitch, DPU/SmartNIC, rack topology,
multi-GPU, remote memory, and distributed shared memory are **not** physically
exercised and are **not** claimed. Single-host results are labelled REAL; multi-node
semantics are modelled as SYNTHETIC.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.