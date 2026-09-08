// Disaggregation Fabric — real single-host proof.
// REAL: system discovery (CPU/NUMA/memory/storage/NIC) on this machine; real host
// memory alloc/write/verify/free; real bounded temp-file storage write/read/verify/
// delete; real loopback TCP transfer; single-host composition. Physical multi-node
// disaggregation is UNSUPPORTED on this host.
#include "test_util.hpp"
#include "dfabric/engine.hpp"
#include "dfabric/socket.hpp"
#include "system_backend.hpp"
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <chrono>
#include <fstream>

using namespace dfabric;
using namespace dfabric::net;

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0); std::setvbuf(stderr, nullptr, _IONBF, 0);

  // --- REAL discovery -------------------------------------------------------
  EngineConfig cfg; cfg.coordinator = CoordinatorId(5); cfg.epoch = CoordinatorEpoch(1);
  FabricEngine e(cfg);
  auto sys = std::make_shared<SystemBackend>();
  e.AddBackend(sys);
  auto d = e.Discover();
  CHECK(d.nodes.size() >= 1);
  CHECK(d.resources.size() >= 4);
  printf("REAL discovery: %zu nodes %zu resources (%s)\n", d.nodes.size(), d.resources.size(), sys->Describe().c_str());
  for (auto& r : d.resources) {
    printf("  res %s class=%s capacity_compute=%f\n", r.id.ToString().c_str(), ToString(r.resource_class), r.capacity.compute_units);
  }

  // --- single-host composition ---------------------------------------------
  CompositionRequest req;
  req.compute_units = 1; req.host_memory_bytes = 1024; req.storage_bytes = 1024; req.network_endpoint_count = 1;
  req.mandatory_classes.insert(ResourceClass::COMPUTE);
  req.mandatory_classes.insert(ResourceClass::HOST_MEMORY);
  req.mandatory_classes.insert(ResourceClass::STORAGE);
  req.mandatory_classes.insert(ResourceClass::NETWORK_ENDPOINT);
  auto cr = e.Compose(req);
  CHECK(cr.ok());
  auto rr = e.Reserve(cr.plan); CHECK(rr.ok());
  auto cc = e.Commit(rr.reservation); CHECK(cc.ok());
  auto ar = e.Activate(cc.composition.id); CHECK(ar.ok());
  CHECK(ar.composition.lifecycle == CompositionLifecycle::ACTIVE);
  printf("REAL single-host composition ACTIVE (id=%llu)\n", (unsigned long long)cc.composition.id.Value());

  // --- REAL host memory: allocate/write/verify/free ------------------------
  {
    const std::size_t n = 1u << 20;   // 1M doubles
    double* buf = static_cast<double*>(std::malloc(n * sizeof(double)));
    CHECK(buf != nullptr);
    for (std::size_t i = 0; i < n; ++i) buf[i] = static_cast<double>(i & 0xffff);
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) sum += buf[i];
    double expect = 0.0;
    for (std::size_t i = 0; i < n; ++i) expect += static_cast<double>(i & 0xffff);
    CHECK(sum == expect);
    std::free(buf);
    printf("REAL host-memory proof ok (%zu bytes)\n", n * sizeof(double));
  }

  // --- REAL storage: bounded temp file write/read/verify/delete -------------
  {
    const char* path = "_system_real.tmp";
    const std::size_t n = 1u << 18;
    std::vector<char> data(n);
    for (std::size_t i = 0; i < n; ++i) data[i] = static_cast<char>((i * 31) & 0xff);
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    ofs.write(data.data(), n); ofs.close();
    std::ifstream ifs(path, std::ios::binary);
    std::vector<char> back(n);
    ifs.read(back.data(), n); ifs.close();
    CHECK(memcmp(data.data(), back.data(), n) == 0);
    std::remove(path);
    printf("REAL storage proof ok (%zu bytes)\n", n);
  }

  // --- REAL loopback TCP transfer -------------------------------------------
  {
    const int port = 55400;
    ServerSocket srv; CHECK(srv.Listen(port));
    std::thread acceptor([&]() {
      TcpSocket c;
      for (int i = 0; i < 400; ++i) { auto t2 = srv.Accept(); if (t2.Valid()) { c = std::move(t2); break; } std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
      if (!c.Valid()) return;
      std::uint8_t buf[4]; int got = 0;
      while (got < 4) { long r = c.RecvSome(buf + got, 4 - got); if (r == -2) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; } if (r <= 0) break; got += (int)r; }
      for (std::size_t off = 0; off < 4; ) { long r = c.SendSome(buf + off, (long)(4 - off)); if (r > 0) off += (std::size_t)r; }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto cli = TcpSocket::Connect("127.0.0.1", port);
    const std::uint8_t out[4] = { 85, 1, 2, 3 };
    for (std::size_t off = 0; off < 4; ) { long r = cli.SendSome(out + off, (long)(4 - off)); if (r > 0) off += (std::size_t)r; }
    std::uint8_t in[4] = { 0, 0, 0, 0 }; int got = 0;
    while (got < 4) { long r = cli.RecvSome(in + got, 4 - got); if (r == -2) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; } if (r <= 0) break; got += (int)r; }
    CHECK(memcmp(out, in, 4) == 0);
    acceptor.join();
    cli.Close(); srv.Close();
    printf("REAL loopback network proof ok\n");
  }

  // --- UNSUPPORTED matrix ---------------------------------------------------
  printf("UNSUPPORTED: physical multi-node disaggregation on this single workstation\n");
  printf("SYNTHETIC: multi-node composition semantics are modelled separately\n");
  printf("REAL: single-host discovery and backend operations above\n");

  printf("system real proof ok\n");
  return 0;
}