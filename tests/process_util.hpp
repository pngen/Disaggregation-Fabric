// Disaggregation Fabric — Windows process helper (real OS process control).
// Retains PROCESS_INFORMATION so we can prove actual death and never claim a
// kill we did not observe. Uses CREATE_NO_WINDOW for unattended operation.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <cstdio>
#include <vector>

namespace dfabric {
namespace test {

struct Proc {
  HANDLE hProcess = nullptr;
  HANDLE hThread = nullptr;
  DWORD pid = 0;
  bool valid() const { return hProcess != nullptr; }
  void Close() {
    if (hThread) { CloseHandle(hThread); hThread = nullptr; }
    if (hProcess) { CloseHandle(hProcess); hProcess = nullptr; }
  }
  bool Kill() {
    if (!valid()) return false;
    return TerminateProcess(hProcess, 1) != 0;
  }
  bool IsAlive() {
    if (!valid()) return false;
    DWORD code = 0;
    if (!GetExitCodeProcess(hProcess, &code)) return false;
    return code == STILL_ACTIVE;
  }
  DWORD Wait(DWORD ms) { return WaitForSingleObject(hProcess, ms); }
};

// Spawn a process with stderr/stdout redirected to a file (for diagnostics).
Proc SpawnToFile(const std::string& exePath, const std::vector<std::string>& args, const std::string& cwd, const std::string& outPath) {
  Proc proc;
  std::string cmdline = "\"" + exePath + "\"";
  for (const auto& a : args) cmdline += " \"" + a + "\"";
  std::vector<char> cmd(cmdline.begin(), cmdline.end());
  cmd.push_back('\0');
  STARTUPINFOA si{}; si.cb = sizeof(si);
  SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
  HANDLE hf = CreateFileA(outPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hf != INVALID_HANDLE_VALUE) { si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = hf; si.hStdError = hf; }
  PROCESS_INFORMATION pi{};
  auto cwdir = cwd.empty() ? nullptr : const_cast<char*>(cwd.c_str());
  if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, cwdir, &si, &pi)) {
    std::fprintf(stderr, "spawn failed for %s\n", exePath.c_str());
    if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);
    return proc;
  }
  if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);
  proc.hProcess = pi.hProcess; proc.hThread = pi.hThread; proc.pid = pi.dwProcessId;
  return proc;
}

// Spawn a process with an argument list (no shell). CREATE_NO_WINDOW.
Proc Spawn(const std::string& exePath, const std::vector<std::string>& args, const std::string& cwd) {
  Proc proc;
  std::string cmdline = "\"" + exePath + "\"";
  for (const auto& a : args) cmdline += " \"" + a + "\"";
  std::vector<char> cmd(cmdline.begin(), cmdline.end());
  cmd.push_back('\0');
  STARTUPINFOA si{}; si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  auto cwdir = cwd.empty() ? nullptr : const_cast<char*>(cwd.c_str());
  if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, cwdir, &si, &pi)) {
    std::fprintf(stderr, "spawn failed for %s\n", exePath.c_str());
    return proc;
  }
  proc.hProcess = pi.hProcess;
  proc.hThread = pi.hThread;
  proc.pid = pi.dwProcessId;
  return proc;
}

}  // namespace test
}  // namespace dfabric