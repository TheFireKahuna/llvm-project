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

// libc-wide stack-overflow handler reserve.
//
// When a thread takes a guard-page hit, the kernel page-fault handler reads
// TEB->GuaranteedStackBytes and either (a) commits more stack and continues
// if the remaining reserved range is at least that large, or (b) raises
// STATUS_STACK_OVERFLOW *while leaving GuaranteedStackBytes worth of stack
// uncommitted-but-reserved* so the SEH dispatcher, our master VEH, the
// signal transport, and (if SA_ONSTACK) the alt-stack switch trampoline
// have somewhere to run. Without a non-zero guarantee, the SEH dispatcher
// gets a single guard page (~4 KB) of headroom, which our own VEH chain +
// crash-backtrace formatter can blow through before the SIGSEGV handler
// ever fires — the process dies on a recursive guard-page fault inside
// the dispatcher with no diagnostic.
//
// 8 KB was chosen empirically to cover: SEH dispatcher (~1 KB), master VEH
// (~512 B), filter chain dispatch (~512 B), signal transport + dispatch
// engine (~1 KB), posix_stack_walk (~2.5 KB ceiling per its comment),
// crash_backtrace_from_context formatter (~1 KB), alt-stack trampoline
// switch (~256 B). The remainder is slack so future additions to the
// crash path don't silently overrun.
inline constexpr uint32_t LIBC_STACK_GUARANTEE_BYTES = 8 * 1024;

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

// Set TEB->GuaranteedStackBytes to LIBC_STACK_GUARANTEE_BYTES for the
// calling thread. Equivalent to kernel32!SetThreadStackGuarantee, written
// as a direct TEB store — no Win32, no syscall, no DLL import. Named
// differently from the Win32 export to avoid any chance of symbol
// collision.
//
// Semantics:
//   - Grow-only. If the current guarantee is already at least our value,
//     no-op. Preserves any prior guarantee installed by user code or by a
//     library loaded under us.
//   - Bounds-checked. If the guarantee region would extend past the bottom
//     of the reserved stack (DeallocationStack), the write is skipped —
//     mirrors kernel32's check, where a too-large guarantee is silently
//     ignored rather than failing the call. Threads with an unusually
//     small reservation (e.g. fiber stacks, custom-stack callers) are
//     skipped rather than partially applied.
//   - Idempotent. Safe to call multiple times on the same thread.
//   - Lock-free. Single 32-bit store; per-thread state.
//
// The guarantee is enforced by the kernel page-fault handler — no further
// action is needed in user mode. Must be called once per thread, early
// enough that any subsequent stack-overflow can land on a guard page that
// still leaves LIBC_STACK_GUARANTEE_BYTES of stack reserved for the SEH
// dispatcher and our VEH chain to run in.
LIBC_INLINE void apply_libc_stack_guarantee() {
  uintptr_t stack_base;
  uintptr_t dealloc_stack;
  uint32_t current;

#ifdef __x86_64__
  // TEB offsets (x64): NtTib.StackBase=0x08, DeallocationStack=0x1478,
  //                    GuaranteedStackBytes=0x1748
  __asm__ __volatile__("movq %%gs:0x08, %0" : "=r"(stack_base));
  __asm__ __volatile__("movq %%gs:0x1478, %0" : "=r"(dealloc_stack));
  __asm__ __volatile__("movl %%gs:0x1748, %0" : "=r"(current));
#elif defined(__aarch64__)
  __asm__ __volatile__("ldr %0, [x18, #0x08]" : "=r"(stack_base));
  __asm__ __volatile__("ldr %0, [x18, #0x1478]" : "=r"(dealloc_stack));
  __asm__ __volatile__("ldr %w0, [x18, #0x1748]" : "=r"(current));
#endif

  if (current >= LIBC_STACK_GUARANTEE_BYTES)
    return;
  if (stack_base - LIBC_STACK_GUARANTEE_BYTES <= dealloc_stack)
    return;

#ifdef __x86_64__
  __asm__ __volatile__("movl %0, %%gs:0x1748" ::"r"(
      LIBC_STACK_GUARANTEE_BYTES));
#elif defined(__aarch64__)
  __asm__ __volatile__("str %w0, [x18, #0x1748]" ::"r"(
      LIBC_STACK_GUARANTEE_BYTES));
#endif
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TEB_FIXUP_H
