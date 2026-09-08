// Disaggregation Fabric — real CUDA proof (RTX 5090).
// REAL: this machine's NVIDIA GPU. The runtime composes a REAL GPU + REAL host
// memory into an authoritative single-host resource set and gates a real CUDA
// launch. A stale/withdrawn GPU is rejected before cudaMalloc. Device memory
// baseline is restored around the kernel.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cuda_runtime.h>
#include "test_util.hpp"
#include "dfabric/engine.hpp"
#include "system_backend.hpp"
#include <memory>
#include <vector>

using namespace dfabric;

#define CK(X) do { cudaError_t e_ = (X); if (e_ != cudaSuccess) { std::printf("CUDA error %d: %s\n", (int)e_, cudaGetErrorString(e_)); return 1; } } while(0)

static __global__ void add_kernel(const float* a, const float* b, float* out, int n) { int i = blockIdx.x * blockDim.x + threadIdx.x; if (i < n) out[i] = a[i] + b[i]; }

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0); std::setvbuf(stderr, nullptr, _IONBF, 0);
  int dev = 0; cudaDeviceProp prop{};
  CK(cudaGetDeviceProperties(&prop, dev));
  float cardMemGB = prop.totalGlobalMem / (1024.0f*1024.0f*1024.0f);
  printf("REAL CUDA device %d: %s (SM %d.%d, %.1f GiB)\n", dev, prop.name, prop.major, prop.minor, cardMemGB);

  // Build an engine + system backend (host memory) + a CUDA-backed GPU resource.
  EngineConfig cfg; cfg.coordinator = CoordinatorId(11); cfg.epoch = CoordinatorEpoch(1);
  FabricEngine e(cfg);
  auto sys = std::make_shared<SystemBackend>();
  e.AddBackend(sys);
  auto d = e.Discover();

  // Add the real GPU as an ACCELERATOR resource on the same host node.
  Resource gpu;
  gpu.id = ResourceId(9200); gpu.generation = ResourceGeneration(1);
  gpu.resource_class = ResourceClass::ACCELERATOR; gpu.node_id = NodeId(9000);
  gpu.provider = "nvidia"; gpu.health = ResourceHealth::HEALTHY; gpu.readiness = Readiness::READY; gpu.reachability = Reachability::REACHABLE;
  gpu.capacity.compute_units = 120.0; gpu.capacity.accelerator_memory_bytes = (double)prop.totalGlobalMem;
  gpu.evidence.id = EvidenceId(9201); gpu.evidence.generation = EvidenceGeneration(1);
  gpu.evidence.source = EvidenceSource::DIRECT; gpu.evidence.state = EvidenceState::FRESH;
  gpu.evidence.timestamp_ms = 1000; gpu.evidence.max_age_ms = 3600000; gpu.evidence.confidence = 0.95; gpu.evidence.durable = true;
  gpu.failure_domain.id = FailureDomainId(9001); gpu.failure_domain.type = FailureDomainType::HOST; gpu.failure_domain.name = "single-host"; gpu.failure_domain.synthetic = false;
  Capability c1; c1.kind = CapabilityKind::CUDA_EXECUTION; c1.state = CapabilityState::SUPPORTED; c1.generation = CapabilityGeneration(1); c1.evidence = gpu.evidence.id;
  gpu.capabilities.push_back(c1);
  char arch[32]; snprintf(arch, sizeof(arch), "%d.%d", prop.major, prop.minor);
  Capability c2; c2.kind = CapabilityKind::ARCH_GENERATION; c2.state = CapabilityState::SUPPORTED; c2.generation = CapabilityGeneration(1); c2.evidence = gpu.evidence.id; c2.detail = arch;
  gpu.capabilities.push_back(c2);
  CHECK(e.RegisterResource(gpu));

  // Compose an authoritative single-host GPU+host-memory set.
  CompositionRequest req;
  req.compute_units = 1; req.accelerator_count = 1; req.accelerator_memory_bytes = (double)prop.totalGlobalMem;
  req.host_memory_bytes = 1024; req.accelerator_architectures.push_back(arch);
  req.mandatory_classes.insert(ResourceClass::COMPUTE);
  req.mandatory_classes.insert(ResourceClass::ACCELERATOR);
  req.mandatory_classes.insert(ResourceClass::HOST_MEMORY);
  auto cr = e.Compose(req);
  CHECK(cr.ok());
  auto rr = e.Reserve(cr.plan); CHECK(rr.ok());
  auto cc = e.Commit(rr.reservation); CHECK(cc.ok());
  auto ar = e.Activate(cc.composition.id); CHECK(ar.ok());
  CHECK(ar.composition.lifecycle == CompositionLifecycle::ACTIVE);
  printf("REAL CUDA composition ACTIVE id=%llu (authoritative)\n", (unsigned long long)cc.composition.id.Value());

  // Stale/withdrawn GPU must be rejected before any CUDA launch.
  {
    Resource withdrawn = gpu; withdrawn.health = ResourceHealth::OFFLINE; withdrawn.evidence.state = EvidenceState::STALE;
    e.OnResourceUpdate(withdrawn);
    e.OnResourceFailure(gpu.id);
    auto after = e.GetComposition(cc.composition.id);
    CHECK(after->lifecycle == CompositionLifecycle::RECOMPOSITION_REQUIRED);
    CHECK(!e.Revalidate(*after).ok());   // stale composition must not authorize a launch
    printf("stale/withdrawn GPU rejected before CUDA launch\n");
  }

  // Restore a healthy GPU + fresh evidence; release the invalidated set, then
  // build a new authoritative set under fresh authority.
  {
    e.RegisterResource(gpu);
    CHECK(e.Release(cc.composition.id));   // fence + free the stale composition holds
    // Re-compose on the refreshed resource.
    CompositionRequest req2 = req;
    auto cr2 = e.Compose(req2); CHECK(cr2.ok());
    auto rr2 = e.Reserve(cr2.plan); CHECK(rr2.ok());
    auto cc2 = e.Commit(rr2.reservation); CHECK(cc2.ok());
    auto ar2 = e.Activate(cc2.composition.id); CHECK(ar2.ok());
    CHECK(ar2.composition.lifecycle == CompositionLifecycle::ACTIVE);

    // Only now is a GPU action authorized.
    size_t freeB, totalB; CK(cudaMemGetInfo(&freeB, &totalB));
    const int n = 1 << 20;
    float *a, *b, *out;
    CK(cudaMalloc(&a, n * sizeof(float))); CK(cudaMalloc(&b, n * sizeof(float))); CK(cudaMalloc(&out, n * sizeof(float)));
    std::vector<float> ha(n), hb(n), hout(n);
    for (int i = 0; i < n; ++i) { ha[i] = (float)i; hb[i] = (float)(i * 2); hout[i] = 0.0f; }
    CK(cudaMemcpy(a, ha.data(), n*sizeof(float), cudaMemcpyHostToDevice));
    CK(cudaMemcpy(b, hb.data(), n*sizeof(float), cudaMemcpyHostToDevice));
    int threads = 256, blocks = (n + threads - 1) / threads;
    add_kernel<<<blocks, threads>>>(a, b, out, n);
    CK(cudaDeviceSynchronize());
    CK(cudaMemcpy(hout.data(), out, n*sizeof(float), cudaMemcpyDeviceToHost));
    for (int i = 0; i < n; ++i) if (hout[i] != ha[i] + hb[i]) { std::printf("CPU parity FAIL at %d: %f != %f\n", i, hout[i], ha[i]+hb[i]); return 1; }
    CK(cudaFree(a)); CK(cudaFree(b)); CK(cudaFree(out));
    size_t freeB2, totalB2; CK(cudaMemGetInfo(&freeB2, &totalB2));
    CHECK(freeB2 == freeB);   // device memory baseline restored
    printf("REAL CUDA proof ok: cudaMalloc/H2D/kernel/sync/D2H/CPU parity/cudaFree; baseline restored\n");
    printf("REAL CUDA DONE\n");
  }
  return 0;
}