//===-- TEB stack bounds fixup for Windows ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Fixes TEB StackBase/StackLimit/DeallocationStack after a stack switch.
// Required after fork (child resumes on parent's COW stack, not the thread's
// initial stack), and useful for future swapcontext/makecontext support.
//
// The VEH handler reads gs:0x10 (StackLimit) for stack overflow detection
// via get_stack_limit(). If TEB fields are stale, stack overflow signals
// misfire silently.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TEB_FIXUP_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TEB_FIXUP_H

#include "src/__support/OSUtil/windows/nt/nt_memory_helpers.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

// Query the actual stack region from the current RSP/SP and update TEB.
// Must run before any code that relies on TEB stack bounds (VEH, SEH,
// RtlCaptureContext, guard page handling).
LIBC_INLINE void fix_teb_stack_bounds() {
  void *sp;
#ifdef __x86_64__
  __asm__ __volatile__("movq %%rsp, %0" : "=r"(sp));
#elif defined(__aarch64__)
  __asm__ __volatile__("mov %0, sp" : "=r"(sp));
#endif

  MEMORY_BASIC_INFORMATION mbi;
  NTSTATUS st = nt_helpers::query_basic_info(sp, mbi);
  if (!NT_SUCCESS(st))
    return;

  auto alloc_base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
  auto region_end =
      reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;

#ifdef __x86_64__
  // TEB offsets (x64): StackBase=0x08, StackLimit=0x10,
  //                    DeallocationStack=0x1478
  __asm__ __volatile__("movq %0, %%gs:0x08" ::"r"(region_end));
  __asm__ __volatile__("movq %0, %%gs:0x10" ::"r"(alloc_base));
  __asm__ __volatile__("movq %0, %%gs:0x1478" ::"r"(alloc_base));
#elif defined(__aarch64__)
  __asm__ __volatile__("str %0, [x18, #0x08]" ::"r"(region_end));
  __asm__ __volatile__("str %0, [x18, #0x10]" ::"r"(alloc_base));
  __asm__ __volatile__("str %0, [x18, #0x1478]" ::"r"(alloc_base));
#endif
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TEB_FIXUP_H
