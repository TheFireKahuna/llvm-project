//===-- backtrace / backtrace_symbols implementation ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX/glibc-compatible backtrace API on NT primitives.
//
// backtrace()            — custom stack walker (x64) / RtlCaptureStackBackTrace
// backtrace_symbols()    — RtlPcToFileHeader + PE export table walk
// backtrace_symbols_fd() — async-signal-safe write to fd via NtWriteFile
//
//===----------------------------------------------------------------------===//

#include "backtrace.h"
#include "bt_format.h"
#include "stack_walker.h"

#include "src/__support/CPP/string_view.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/nt/nt_process_types.h"
#include "src/__support/macros/config.h"
#include "src/stdlib/free.h"
#include "src/stdlib/malloc.h"

namespace LIBC_NAMESPACE_DECL {

// =========================================================================
// Public API
// =========================================================================

namespace internal {

[[gnu::noinline]]
int posix_backtrace(void **buffer, int size) {
  if (size <= 0)
    return 0;

#ifdef __x86_64__
  // Capture lightweight register state for the custom stack walker.
  // Only callee-saved GPRs + RSP + RIP — 144 bytes vs 1232 for CONTEXT.
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

  // Skip 2 frames: this function + the public backtrace() entrypoint wrapper.
  // buffer[0] = the caller of backtrace(), matching glibc convention.
  return posix_stack_walk(buffer, size, /*skip=*/2, state);
#else
  // AArch64 fallback: use the NT API directly.
  return static_cast<int>(
      ::RtlCaptureStackBackTrace(2, static_cast<ULONG>(size), buffer, nullptr));
#endif
}

char **posix_backtrace_symbols(void *const *buffer, int size) {
  if (size <= 0)
    return nullptr;

  constexpr int kMaxFrameLen = 512;

  // First pass: format all frames to measure total string storage.
  struct FrameLen {
    int len;
  };

  // Stack-allocate for up to 128 frames.
  constexpr int kStackFrames = 128;
  FrameLen stack_info[kStackFrames];
  FrameLen *info = (size <= kStackFrames)
                       ? stack_info
                       : static_cast<FrameLen *>(LIBC_NAMESPACE::malloc(
                             static_cast<size_t>(size) * sizeof(FrameLen)));
  if (!info)
    return nullptr;

  char scratch[kMaxFrameLen];
  size_t total_strings = 0;

  for (int i = 0; i < size; ++i) {
    int len = bt_fmt::format_frame(buffer[i], scratch, kMaxFrameLen);
    info[i].len = len;
    total_strings += static_cast<size_t>(len) + 1;
  }

  // Single allocation: char*[size] + string data.
  size_t ptrs_size = static_cast<size_t>(size) * sizeof(char *);
  size_t total = ptrs_size + total_strings;

  auto *result = static_cast<char **>(LIBC_NAMESPACE::malloc(total));
  if (!result) {
    if (info != stack_info)
      LIBC_NAMESPACE::free(info);
    return nullptr;
  }

  // Second pass: format into the allocated block.
  char *str_area = reinterpret_cast<char *>(result) + ptrs_size;
  for (int i = 0; i < size; ++i) {
    result[i] = str_area;
    int len = bt_fmt::format_frame(buffer[i], str_area, kMaxFrameLen);
    str_area[len] = '\0';
    str_area += len + 1;
  }

  if (info != stack_info)
    LIBC_NAMESPACE::free(info);

  return result;
}

void posix_backtrace_symbols_fd(void *const *buffer, int size, int fd) {
  if (size <= 0)
    return;

  // Resolve fd to NT handle.
  auto handle_or = fd_table.get(fd);
  if (!handle_or.has_value())
    return;
  HANDLE h = handle_or.value();

  constexpr int kMaxFrameLen = 512;
  char line[kMaxFrameLen + 1]; // +1 for newline

  for (int i = 0; i < size; ++i) {
    int len = bt_fmt::format_frame(buffer[i], line, kMaxFrameLen);
    if (len > 0) {
      line[len] = '\n';
      bt_fmt::write_to_handle(h,
                               cpp::string_view(line, static_cast<size_t>(len) + 1));
    }
  }
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
