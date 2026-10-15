// Diagnostic only: on a guest-memory read fault, append the host RIP and a crude stack scan (module RVAs)
// to crash_report.txt in the working directory. Symbolize the RVAs with prototype2.map
// (tools/symbolize_crash.py). Returns CONTINUE_SEARCH, so the runtime's own handling is unchanged.
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace {
std::atomic<int> g_reports{0};
uintptr_t g_image_begin = 0, g_image_end = 0;

LONG CALLBACK OnException(EXCEPTION_POINTERS* info) {
  const EXCEPTION_RECORD* rec = info->ExceptionRecord;
  if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const uintptr_t kind = rec->ExceptionInformation[0];  // 0 read, 1 write, 8 execute
  const uintptr_t addr = rec->ExceptionInformation[1];
  // Guest arena is mapped at 0x1'00000000 (virtual) and 0x2'00000000 (physical). Write faults there are the
  // GPU/memory write-watch mechanism, so only reads and executes are reported.
  if (kind == 1 || addr < 0x100000000ull || addr >= 0x300000000ull) return EXCEPTION_CONTINUE_SEARCH;
  if (g_reports.fetch_add(1) >= 3) return EXCEPTION_CONTINUE_SEARCH;
  FILE* f = std::fopen("crash_report.txt", "a");
  if (!f) return EXCEPTION_CONTINUE_SEARCH;
  const CONTEXT* c = info->ContextRecord;
  std::fprintf(f, "fault kind=%llu host=%016llX guest=%08llX thread=%lu\n", (unsigned long long)kind,
               (unsigned long long)addr, (unsigned long long)(addr & 0xFFFFFFFFull), GetCurrentThreadId());
  auto rva = [](uintptr_t p) { return (p >= g_image_begin && p < g_image_end) ? p - g_image_begin : 0; };
  std::fprintf(f, "rip rva=%llX\n", (unsigned long long)rva(c->Rip));
  std::fprintf(f, "regs rax=%llX rbx=%llX rcx=%llX rdx=%llX rsi=%llX rdi=%llX r8=%llX r9=%llX\n", c->Rax, c->Rbx,
               c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->R8, c->R9);
  const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(c->Rsp);
  MEMORY_BASIC_INFORMATION mbi{};
  int limit = 0;
  if (VirtualQuery(sp, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT) {
    const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    limit = static_cast<int>((end - reinterpret_cast<uintptr_t>(sp)) / sizeof(uintptr_t));
  }
  int printed = 0;
  for (int i = 0; i < limit && i < 8192 && printed < 40; ++i) {
    if (uintptr_t r = rva(sp[i])) {
      std::fprintf(f, "stack[%d] rva=%llX\n", i, (unsigned long long)r);
      ++printed;
    }
  }
  std::fprintf(f, "----\n");
  std::fclose(f);
  return EXCEPTION_CONTINUE_SEARCH;
}

struct Installer {
  Installer() {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    g_image_begin = reinterpret_cast<uintptr_t>(base);
    g_image_end = g_image_begin + nt->OptionalHeader.SizeOfImage;
    AddVectoredExceptionHandler(1, OnException);
  }
} g_installer;
}  // namespace
