// Disaggregation Fabric — regression: a coordinator restarted on its durable state moves to an
// epoch above the one it recovered, even when started with the same --epoch.
// REAL: coordinator as an OS process over real framed TCP; real OS kill and restart.
#include "process_util.hpp"
#include "dfabric/protocol.hpp"
#include "dfabric/socket.hpp"
#include "dfabric/codec.hpp"
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace dfabric;
using namespace dfabric::proto;
using namespace dfabric::net;
using namespace dfabric::test;

static int g_failures = 0;
#define EXPECT(cond) do { if (!(cond)) { ++g_failures; std::printf("FAILED: %s (%s:%d)\n", #cond, __FILE__, __LINE__); } } while (0)

static int FreePort() {
  WsaInit();
  SOCKET s = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
  int port = 0;
  if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
    int len = sizeof(a);
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0) port = ntohs(a.sin_port);
  }
  closesocket(s);
  return port;
}

// Connects once the coordinator listens; gives up only if the coordinator process exited.
static bool ConnectTo(Proc& coord, int port, TcpSocket& out) {
  for (;;) {
    try { out = TcpSocket::Connect("127.0.0.1", port); return true; } catch (...) {}
    if (!coord.IsAlive()) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

// Sends a request and waits for its reply; ends only on the reply or a broken connection.
static bool Request(TcpSocket& c, FrameDecoder& dec, FrameType rt, FrameType st, std::uint64_t corr,
                    const std::vector<std::uint8_t>& pl, Frame& out) {
  auto fr = Encode(rt, corr, pl.data(), pl.size());
  std::size_t off = 0;
  while (off < fr.size()) {
    long r = c.SendSome(fr.data() + off, static_cast<long>(fr.size() - off));
    if (r < 0) return false;
    if (r > 0) off += static_cast<std::size_t>(r); else std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::uint8_t buf[8192];
  for (;;) {
    long r = c.RecvSome(buf, sizeof(buf));
    if (r == -2) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
    if (r <= 0) return false;
    std::vector<Frame> frames; std::string err;
    if (!dec.Feed(buf, static_cast<std::size_t>(r), frames, err)) return false;
    for (auto& f : frames) if (f.type == st && f.correlation == corr) { out = std::move(f); return true; }
  }
}

static std::uint64_t QueryEpoch(TcpSocket& c, FrameDecoder& dec, std::uint64_t corr) {
  Frame resp; WireWriter w;
  if (!Request(c, dec, FrameType::QUERY_STATE, FrameType::STATE_RESP, corr, w.Take(), resp)) return 0;
  WireReader rd(resp.payload.data(), resp.payload.size());
  return rd.U64();
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  char exeBuf[MAX_PATH]; GetModuleFileNameA(nullptr, exeBuf, MAX_PATH);
  char fullBuf[MAX_PATH]; GetFullPathNameA(exeBuf, MAX_PATH, fullBuf, nullptr);
  const std::string exeDir = std::string(fullBuf).substr(0, std::string(fullBuf).find_last_of("\\"));
  const std::string coord = exeDir + "\\dsh_coordinator.exe";
  const std::string state = "_regression_restart_state.bin";
  std::remove(state.c_str());
  const int port = FreePort();
  EXPECT(port != 0);
  const std::vector<std::string> args = { "--port=" + std::to_string(port), "--epoch=1", "--state=" + state };

  std::uint64_t first = 0;
  Proc c1 = SpawnToFile(coord, args, "", "_rr_c1.out");
  EXPECT(c1.valid());
  {
    TcpSocket s;
    EXPECT(ConnectTo(c1, port, s));
    FrameDecoder dec; Frame resp; WireWriter w;
    EXPECT(Request(s, dec, FrameType::SAVE_REQ, FrameType::SAVE_RESP, 1, w.Take(), resp));
    first = QueryEpoch(s, dec, 2);
    EXPECT(first == 1);
  }
  EXPECT(c1.Kill());
  c1.Wait(INFINITE);
  c1.Close();

  // Restarted on the saved state with the same --epoch.
  Proc c2 = SpawnToFile(coord, args, "", "_rr_c2.out");
  EXPECT(c2.valid());
  {
    TcpSocket s;
    EXPECT(ConnectTo(c2, port, s));
    FrameDecoder dec;
    const std::uint64_t second = QueryEpoch(s, dec, 3);
    std::printf("epoch before restart %llu, after %llu\n", (unsigned long long)first, (unsigned long long)second);
    EXPECT(second > first);
  }
  EXPECT(c2.Kill());
  c2.Wait(INFINITE);
  c2.Close();

  std::remove(state.c_str());
  std::remove("_rr_c1.out");
  std::remove("_rr_c2.out");
  std::printf("regression_restart: %d failure(s)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
