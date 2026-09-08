// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/CrashReport.h"

#include <cstddef>
#include <cstring>
#include <ctime>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#elif !defined(ANDROID)
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/Version.h"

namespace Common::CrashReport
{
#ifdef ANDROID
// Android has tombstones (and the CI keeps the unstripped libmain.so), and
// bionic has no <execinfo.h>: nothing to install.
void Install(const std::string& /*directory*/)
{
}
#else
namespace
{
// Fixed at Install() so the handlers never allocate, lock, or call into
// anything that may be mid-crash. Every buffer below lives on the stack or in
// static storage; the formatting helpers are deliberately trivial.
char s_directory[1024] = {};
char s_version[128] = {};

size_t Put(char* buf, size_t cap, size_t pos, const char* s)
{
  while (*s != '\0' && pos + 1 < cap)
    buf[pos++] = *s++;
  buf[pos] = '\0';
  return pos;
}

size_t PutHex(char* buf, size_t cap, size_t pos, u64 value)
{
  static const char digits[] = "0123456789abcdef";
  char tmp[19];
  int n = 0;
  do
  {
    tmp[n++] = digits[value & 0xF];
    value >>= 4;
  } while (value != 0 && n < 16);
  pos = Put(buf, cap, pos, "0x");
  while (n > 0 && pos + 1 < cap)
    buf[pos++] = tmp[--n];
  buf[pos] = '\0';
  return pos;
}

size_t PutDec(char* buf, size_t cap, size_t pos, u64 value)
{
  char tmp[21];
  int n = 0;
  do
  {
    tmp[n++] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value != 0 && n < 20);
  while (n > 0 && pos + 1 < cap)
    buf[pos++] = tmp[--n];
  buf[pos] = '\0';
  return pos;
}

const char* BaseName(const char* path)
{
  const char* base = path;
  for (const char* p = path; *p != '\0'; ++p)
  {
    if (*p == '/' || *p == '\\')
      base = p + 1;
  }
  return base;
}

// <directory>/crash_YYYYMMDD_HHMMSS.txt -- the same naming as the session logs
// in that folder, so "the newest file" is the crash when one just happened.
void BuildPath(char* out, size_t cap)
{
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  char stamp[32] = "unknown";
  std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
  size_t pos = Put(out, cap, 0, s_directory);
  pos = Put(out, cap, pos, "crash_");
  pos = Put(out, cap, pos, stamp);
  Put(out, cap, pos, ".txt");
}

#ifdef _WIN32
void Write(HANDLE file, const char* s)
{
  DWORD written = 0;
  WriteFile(file, s, static_cast<DWORD>(std::strlen(s)), &written, nullptr);
}

const char* ExceptionName(DWORD code)
{
  switch (code)
  {
  case EXCEPTION_ACCESS_VIOLATION:
    return "access violation";
  case EXCEPTION_STACK_OVERFLOW:
    return "stack overflow";
  case EXCEPTION_ILLEGAL_INSTRUCTION:
    return "illegal instruction";
  case EXCEPTION_PRIV_INSTRUCTION:
    return "privileged instruction";
  case EXCEPTION_IN_PAGE_ERROR:
    return "in-page error";
  case EXCEPTION_INT_DIVIDE_BY_ZERO:
    return "integer divide by zero";
  case EXCEPTION_BREAKPOINT:
    return "breakpoint";
  case 0xE06D7363:
    return "unhandled C++ exception";
  case 0x40000015:
    return "abort() (fatal app exit)";
  default:
    return "exception";
  }
}

LONG WINAPI Filter(EXCEPTION_POINTERS* info)
{
  char path[1200];
  BuildPath(path, sizeof(path));
  const HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return EXCEPTION_CONTINUE_SEARCH;

  char line[600];
  size_t pos = Put(line, sizeof(line), 0, "OrreLink crash report (windows)\nversion=");
  pos = Put(line, sizeof(line), pos, s_version);
  pos = Put(line, sizeof(line), pos, "\nthread=");
  pos = PutDec(line, sizeof(line), pos, GetCurrentThreadId());
  Put(line, sizeof(line), pos, "\n");
  Write(file, line);

  const EXCEPTION_RECORD* record = info->ExceptionRecord;
  pos = Put(line, sizeof(line), 0, "exception=");
  pos = PutHex(line, sizeof(line), pos, record->ExceptionCode);
  pos = Put(line, sizeof(line), pos, " (");
  pos = Put(line, sizeof(line), pos, ExceptionName(record->ExceptionCode));
  pos = Put(line, sizeof(line), pos, ") at ");
  pos = PutHex(line, sizeof(line), pos, reinterpret_cast<u64>(record->ExceptionAddress));
  if ((record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
       record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
      record->NumberParameters >= 2)
  {
    pos = Put(line, sizeof(line), pos,
              record->ExceptionInformation[0] == 1 ? " writing " :
              record->ExceptionInformation[0] == 8 ? " executing " :
                                                     " reading ");
    pos = PutHex(line, sizeof(line), pos, record->ExceptionInformation[1]);
  }
  Put(line, sizeof(line), pos, "\n");
  Write(file, line);

#ifdef _M_X64
  // Walk the faulting thread with the OS unwinder. Each frame prints as
  // module+offset (offset from the module's load address), which the .map/PDB
  // of the same build turns into a function name. The walk itself may fault on
  // a corrupted stack; __try keeps whatever was written so far (the file is
  // written line by line). A stack overflow usually gets no report at all:
  // this filter runs on the exhausted stack.
  CONTEXT ctx = *info->ContextRecord;
  __try
  {
    pos = Put(line, sizeof(line), 0, "rip=");
    pos = PutHex(line, sizeof(line), pos, ctx.Rip);
    pos = Put(line, sizeof(line), pos, " rsp=");
    pos = PutHex(line, sizeof(line), pos, ctx.Rsp);
    pos = Put(line, sizeof(line), pos, " rbp=");
    pos = PutHex(line, sizeof(line), pos, ctx.Rbp);
    Put(line, sizeof(line), pos, "\nbacktrace:\n");
    Write(file, line);

    for (int frame = 0; frame < 64; ++frame)
    {
      const DWORD64 pc = ctx.Rip;
      if (pc == 0)
        break;

      HMODULE hmodule = nullptr;
      GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(pc), &hmodule);
      char name[MAX_PATH] = "?";
      if (hmodule != nullptr)
        GetModuleFileNameA(hmodule, name, MAX_PATH);

      pos = Put(line, sizeof(line), 0, "  #");
      pos = PutDec(line, sizeof(line), pos, static_cast<u64>(frame));
      pos = Put(line, sizeof(line), pos, " pc=");
      pos = PutHex(line, sizeof(line), pos, pc);
      pos = Put(line, sizeof(line), pos, " ");
      pos = Put(line, sizeof(line), pos, BaseName(name));
      pos = Put(line, sizeof(line), pos, "+");
      pos = PutHex(line, sizeof(line), pos,
                   hmodule ? pc - reinterpret_cast<DWORD64>(hmodule) : 0);
      Put(line, sizeof(line), pos, "\n");
      Write(file, line);

      DWORD64 image_base = 0;
      PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(pc, &image_base, nullptr);
      if (function == nullptr)
      {
        // Leaf function without unwind data: the return address is on top of
        // the stack. Stop rather than read an unmapped stack.
        if (IsBadReadPtr(reinterpret_cast<const void*>(ctx.Rsp), sizeof(DWORD64)))
          break;
        ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
        ctx.Rsp += sizeof(DWORD64);
        continue;
      }
      void* handler_data = nullptr;
      DWORD64 establisher_frame = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, pc, function, &ctx, &handler_data,
                       &establisher_frame, nullptr);
      if (ctx.Rip == pc)
        break;
    }
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    Write(file, "  (unwind aborted: the stack walk itself faulted)\n");
  }
#else
  Write(file, "backtrace: (no unwinder on this architecture)\n");
#endif

  CloseHandle(file);
  return EXCEPTION_CONTINUE_SEARCH;
}
#else  // POSIX

const char* SignalName(int sig)
{
  switch (sig)
  {
  case SIGSEGV:
    return "SIGSEGV";
  case SIGBUS:
    return "SIGBUS";
  case SIGILL:
    return "SIGILL";
  case SIGFPE:
    return "SIGFPE";
  case SIGABRT:
    return "SIGABRT";
  case SIGTRAP:
    return "SIGTRAP";
  default:
    return "signal";
  }
}

void WriteAll(int fd, const char* s)
{
  size_t left = std::strlen(s);
  while (left > 0)
  {
    const ssize_t n = write(fd, s, left);
    if (n <= 0)
      return;
    s += n;
    left -= static_cast<size_t>(n);
  }
}

// Post-mortem scratch lives in static storage: the JIT's fault handler
// (MemTools) re-raises to this one on its own ~8 KiB alternate stack.
char s_path[1200];
char s_line[600];
void* s_frames[64];

void Handler(int sig, siginfo_t* info, void* /*context*/)
{
  char* const path = s_path;
  BuildPath(path, sizeof(s_path));
  const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0)
  {
    char* const line = s_line;
    size_t pos = Put(line, sizeof(s_line), 0, "OrreLink crash report (posix)\nversion=");
    pos = Put(line, sizeof(s_line), pos, s_version);
    pos = Put(line, sizeof(s_line), pos, "\nsignal=");
    pos = PutDec(line, sizeof(s_line), pos, static_cast<u64>(sig));
    pos = Put(line, sizeof(s_line), pos, " (");
    pos = Put(line, sizeof(s_line), pos, SignalName(sig));
    pos = Put(line, sizeof(s_line), pos, ") code=");
    pos = PutDec(line, sizeof(s_line), pos, static_cast<u64>(info ? info->si_code : 0));
    pos = Put(line, sizeof(s_line), pos, " addr=");
    pos = PutHex(line, sizeof(s_line), pos,
                 reinterpret_cast<u64>(info ? info->si_addr : nullptr));
    Put(line, sizeof(s_line), pos, "\nbacktrace (module+offset):\n");
    WriteAll(fd, line);

    void** const frames = s_frames;
    const int count = backtrace(frames, 64);
    for (int i = 0; i < count; ++i)
    {
      Dl_info dl{};
      const bool known = dladdr(frames[i], &dl) != 0 && dl.dli_fname != nullptr;
      pos = Put(line, sizeof(s_line), 0, "  #");
      pos = PutDec(line, sizeof(s_line), pos, static_cast<u64>(i));
      pos = Put(line, sizeof(s_line), pos, " pc=");
      pos = PutHex(line, sizeof(s_line), pos, reinterpret_cast<u64>(frames[i]));
      pos = Put(line, sizeof(s_line), pos, " ");
      pos = Put(line, sizeof(s_line), pos, known ? BaseName(dl.dli_fname) : "?");
      pos = Put(line, sizeof(s_line), pos, "+");
      pos = PutHex(line, sizeof(s_line), pos,
                   known ? reinterpret_cast<u64>(frames[i]) - reinterpret_cast<u64>(dl.dli_fbase) :
                           0);
      if (known && dl.dli_sname != nullptr)
      {
        pos = Put(line, sizeof(s_line), pos, " (");
        pos = Put(line, sizeof(s_line), pos, dl.dli_sname);
        pos = Put(line, sizeof(s_line), pos, "+");
        pos = PutHex(line, sizeof(s_line), pos,
                     reinterpret_cast<u64>(frames[i]) - reinterpret_cast<u64>(dl.dli_saddr));
        pos = Put(line, sizeof(s_line), pos, ")");
      }
      Put(line, sizeof(s_line), pos, "\n");
      WriteAll(fd, line);
    }
    WriteAll(fd, "symbolized (best effort):\n");
    backtrace_symbols_fd(frames, count, fd);
    close(fd);
  }
  // Hand the signal back to the default action so the OS still writes its own
  // crash log / core dump exactly as before.
  signal(sig, SIG_DFL);
  raise(sig);
}

// The alternate stack lets the handler run after a stack overflow.
alignas(16) char s_alt_stack[256 * 1024];
#endif
}  // namespace

void Install(const std::string& directory)
{
  File::CreateFullPath(directory);
  Put(s_directory, sizeof(s_directory), 0, directory.c_str());
  Put(s_version, sizeof(s_version), 0, Common::GetScmDescStr().c_str());

#ifdef _WIN32
  SetUnhandledExceptionFilter(Filter);
#else
  // First-use initialisation that must not happen inside the handler: glibc's
  // first backtrace() dlopens libgcc_s (malloc + loader lock) and the first
  // localtime_r() loads the zone data (malloc). A crash while the heap lock is
  // held would otherwise deadlock here instead of dying.
  void* warm[1];
  backtrace(warm, 1);
  BuildPath(s_path, sizeof(s_path));

  stack_t stack{};
  stack.ss_sp = s_alt_stack;
  stack.ss_size = sizeof(s_alt_stack);
  stack.ss_flags = 0;
  sigaltstack(&stack, nullptr);

  struct sigaction action{};
  action.sa_sigaction = Handler;
  action.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&action.sa_mask);
  // Linux: the JIT's fault handler (MemTools) is installed later, saves
  // whatever is here as its "previous" handler, and hands it every fault that
  // is not a fastmem access -- so this still runs for real crashes. macOS: the
  // JIT uses Mach exception ports and never touches sigaction; unhandled
  // faults arrive here as plain SIGSEGV/SIGBUS.
  const int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP};
  for (const int sig : signals)
    sigaction(sig, &action, nullptr);
#endif
}
#endif  // ANDROID
}  // namespace Common::CrashReport
