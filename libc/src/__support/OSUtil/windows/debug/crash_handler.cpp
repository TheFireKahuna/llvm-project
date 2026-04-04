//===-- Crash backtrace handler ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Prints a stack backtrace to stderr on fatal signals. Two entry points:
//
//   crash_backtrace(signum)
//     Called by execute_default_action() for core-dump signals before
//     NtTerminateProcess. Walks from the current call frame.
//
//   crash_backtrace_from_context(signum, rec, ctx)
//     Called from VEH passthrough paths for synchronous faults that we are
//     about to surrender to the OS unhandled-exception terminator. Walks
//     from the captured exception CONTEXT so the printed stack is the user
//     fault site, not the VEH frame. Header carries the fault code + addr.
//
// Async-signal-safe: no heap allocation, no fd_table, no FILE* buffering.
// Writes directly to PEB stderr handle via NtWriteFile.
//
// Stack budget: ~2.5KB (WalkState 144B + frames 2048B + line buffer 513B).
//
//===----------------------------------------------------------------------===//

#include "crash_handler.h"
#include "bt_format.h"
#include "stack_walker.h"

#include "hdr/signal_macros.h"
#include "src/__support/CPP/span.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/CPP/stringstream.h"
#include "src/__support/OSUtil/windows/nt/nt_process_types.h"
#include "src/__support/integer_to_string.h" // IntegerToString, radix::Hex
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Signal number -> short name for the crash header line.
static cpp::string_view signal_name(int signum) {
  switch (signum) {
  case SIGQUIT: return "SIGQUIT";
  case SIGILL:  return "SIGILL";
  case SIGTRAP: return "SIGTRAP";
  case SIGABRT: return "SIGABRT";
  case SIGBUS:  return "SIGBUS";
  case SIGFPE:  return "SIGFPE";
  case SIGSEGV: return "SIGSEGV";
  default:      return "SIG???";
  }
}

namespace {

// Shared frame-formatting tail. Takes pre-captured frame addresses and writes
// each as "#N  module(symbol+0xoffset) [0xaddr]\n" to \p stderr_h. Used by
// both crash_backtrace and crash_backtrace_from_context after their headers.
void write_frames(HANDLE stderr_h, void **frames, int nframes) {
  constexpr int kMaxFrameLen = 512;
  char line[kMaxFrameLen + 1]; // +1 for newline

  for (int i = 0; i < nframes; ++i) {
    char prefix[16];
    cpp::StringStream ps(cpp::span<char>(prefix, sizeof(prefix)));
    ps << '#' << i << "  ";
    bt_fmt::write_to_handle(stderr_h, ps.str());

    int len = bt_fmt::format_frame(frames[i], line, kMaxFrameLen);
    if (len > 0) {
      line[len] = '\n';
      bt_fmt::write_to_handle(stderr_h,
                               cpp::string_view(line, static_cast<size_t>(len) + 1));
    }
  }

  if (nframes == 0)
    bt_fmt::write_to_handle(stderr_h, "(no frames captured)\n");
}

} // namespace

[[gnu::noinline]]
void crash_backtrace(int signum) {
  // Get the PEB stderr handle directly — no fd_table lookup.
  HANDLE stderr_h = NtCurrentStandardError();
  if (!stderr_h)
    return;

  // --- Header line ---
  // Format: "\n*** Fatal signal <N> (<NAME>), backtrace:\n"
  {
    char hdr[128];
    cpp::StringStream ss(cpp::span<char>(hdr, sizeof(hdr)));
    ss << "\n*** Fatal signal " << signum << " (" << signal_name(signum)
       << "), backtrace:\n";
    bt_fmt::write_to_handle(stderr_h, ss.str());
  }

  // --- Stack walk ---
  constexpr int kMaxFrames = 64;
  void *frames[kMaxFrames];
  int nframes = 0;

#ifdef __x86_64__
  WalkState state = {};
  __asm__ __volatile__("leaq (%%rip), %0" : "=r"(state.rip));
  __asm__ __volatile__("movq %%rsp, %0" : "=r"(state.rsp));
  __asm__ __volatile__("movq %%rbx, %0" : "=r"(state.gprs[3]));
  __asm__ __volatile__("movq %%rbp, %0" : "=r"(state.gprs[5]));
  __asm__ __volatile__("movq %%rsi, %0" : "=r"(state.gprs[6]));
  __asm__ __volatile__("movq %%rdi, %0" : "=r"(state.gprs[7]));
  __asm__ __volatile__("movq %%r12, %0" : "=r"(state.gprs[12]));
  __asm__ __volatile__("movq %%r13, %0" : "=r"(state.gprs[13]));
  __asm__ __volatile__("movq %%r14, %0" : "=r"(state.gprs[14]));
  __asm__ __volatile__("movq %%r15, %0" : "=r"(state.gprs[15]));
  state.gprs[4] = state.rsp;

  // Skip 2: crash_backtrace + execute_default_action.
  nframes = posix_stack_walk(frames, kMaxFrames, /*skip=*/2, state);
#else
  nframes = static_cast<int>(
      ::RtlCaptureStackBackTrace(2, kMaxFrames, frames, nullptr));
#endif

  write_frames(stderr_h, frames, nframes);
}

[[gnu::noinline]]
void crash_backtrace_from_context(int signum, const EXCEPTION_RECORD *rec,
                                  const CONTEXT *ctx) {
  HANDLE stderr_h = NtCurrentStandardError();
  if (!stderr_h)
    return;

  // --- Header line ---
  // Format: "\n*** Fatal signal <N> (<NAME>) — exception <code> at PC=<addr>
  //          fault_va=<va>, backtrace:\n"
  // The fault VA only exists for ACCESS_VIOLATION / IN_PAGE_ERROR; for other
  // codes (DIV0, ILL, BREAKPOINT, ...) the second clause is omitted.
  using HexU32 = IntegerToString<uint32_t, radix::Hex::WithPrefix::Uppercase>;
  using HexPtr = IntegerToString<uintptr_t, radix::Hex::WithPrefix::Uppercase>;
  {
    char hdr[256];
    cpp::StringStream ss(cpp::span<char>(hdr, sizeof(hdr)));
    ss << "\n*** Fatal signal " << signum << " (" << signal_name(signum)
       << ") — exception ";
    const HexU32 code_hex(rec ? static_cast<uint32_t>(rec->ExceptionCode)
                              : 0u);
    ss << code_hex.view() << " at PC=";
    void *pc = rec ? rec->ExceptionAddress : nullptr;
    const HexPtr pc_hex(reinterpret_cast<uintptr_t>(pc));
    ss << pc_hex.view();
    if (rec && (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
                rec->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        rec->NumberParameters >= 2) {
      const HexPtr va_hex(
          static_cast<uintptr_t>(rec->ExceptionInformation[1]));
      ss << " fault_va=" << va_hex.view();
    }
    ss << ", backtrace:\n";
    bt_fmt::write_to_handle(stderr_h, ss.str());
  }

  if (!ctx) {
    bt_fmt::write_to_handle(stderr_h, "(no context)\n");
    return;
  }

  constexpr int kMaxFrames = 64;
  void *frames[kMaxFrames];
  int nframes = 0;

#ifdef __x86_64__
  WalkState state = {};
  walk_state_from_context(state, ctx);
  // skip=0: the first frame is the faulting PC itself, which is what the
  // caller actually wants to see (not "this VEH wrapper called us").
  nframes = posix_stack_walk(frames, kMaxFrames, /*skip=*/0, state);
#else
  // AArch64 path — no symbolic walk-from-CONTEXT yet; fall back to the
  // current-frame walker so we at least emit something.
  nframes = static_cast<int>(
      ::RtlCaptureStackBackTrace(0, kMaxFrames, frames, nullptr));
#endif

  write_frames(stderr_h, frames, nframes);
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
