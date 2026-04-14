//===-- Alt-stack trampolines for signal delivery ------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Switches the stack pointer to the alternate signal stack before calling the
// signal handler. The frame pointer anchors the original stack so RtlUnwindEx
// can walk back if the handler longjmps or throws.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/signal/signal_internal.h"

// Dispatch helper called on the alt stack by the asm trampoline. Shared by
// both x86_64 and aarch64 — the logic is arch-independent, only the naked
// trampoline that calls it differs. Outside LIBC_NAMESPACE_DECL because the
// extern "C" naked trampolines that reference it via asm operand are also
// at file scope (they need C linkage for .seh_proc symbol names).
namespace {
void call_signal_handler(int signum, siginfo_t *info,
                         const struct sigaction *action,
                         ucontext_t *context) {
  if (action->sa_flags & SA_SIGINFO)
    action->sa_sigaction(signum, info, context);
  else
    action->sa_handler(signum);
}
} // namespace

// --- Alt-stack trampoline (x86_64) -------------------------------------------
//
// RBP anchors the original stack so RtlUnwindEx can walk back if the handler
// longjmps or throws. The .seh_setframe directive tells the OS unwinder that
// RBP is the frame register, making the stack switch transparent to unwinding.

#ifdef __x86_64__

// extern "C" for a predictable .seh_proc symbol name (naked functions need
// manual SEH unwind info, and .seh_proc requires a known symbol).
//
// Args (Win64): rcx = alt_stack_top (16-aligned), rdx = signum,
//               r8 = siginfo_t*, r9 = const struct sigaction*,
//               [rsp+0x28] = ucontext_t* (5th arg, on stack per Win64 ABI)
extern "C" [[gnu::naked]]
void __llvm_libc_deliver_on_alt_stack(void *, int, siginfo_t *,
                                      const struct sigaction *,
                                      ucontext_t *) {
  asm(R"(
      .seh_proc __llvm_libc_deliver_on_alt_stack
      push %%rbp
      .seh_pushreg %%rbp
      mov %%rsp, %%rbp
      .seh_setframe %%rbp, 0
      .seh_endprologue

      // Load 5th arg (ucontext_t*) from caller's frame before switching stack.
      // Win64: 5th arg at [old_rsp + 0x28]. After push rbp, rbp = old_rsp - 8,
      // so 5th arg is at [rbp + 0x30].
      mov 0x30(%%rbp), %%rax

      // Switch RSP to alternate signal stack. 40 = 32 shadow + 8 alignment
      // (call pushes 8 bytes -> RSP becomes 16-aligned at callee entry).
      mov %%rcx, %%rsp
      sub $40, %%rsp

      // Rearrange args for call_signal_handler(signum, info, action, context).
      // All 4 args fit in registers per Win64 ABI.
      mov %%rdx, %%rcx
      mov %%r8, %%rdx
      mov %%r9, %%r8
      mov %%rax, %%r9
      call %P[handler]

      // Restore original stack
      mov %%rbp, %%rsp
      pop %%rbp
      retq
      .seh_endproc
  )" :: [handler] "X"(call_signal_handler));
}

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {

void deliver_on_alt_stack(void *alt_stack_top, int signum, siginfo_t *info,
                          const struct sigaction *action,
                          ucontext_t *context) {
  __llvm_libc_deliver_on_alt_stack(alt_stack_top, signum, info, action,
                                    context);
}

} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

#endif // __x86_64__

// --- Alt-stack trampoline (aarch64) ------------------------------------------
//
// FP (x29) anchors the original stack so RtlUnwindEx can walk back if the
// handler longjmps or throws. The {x29, x30} pair maintains the frame pointer
// chain required by ETW and debuggers.
//
// PAC: pacibsp signs x30 with the B-key using SP as context, preventing return
// address overwrites. autibsp verifies the signature before return. pacibsp
// also serves as an implicit BTI landing pad (bti c), so no separate BTI hint
// is needed. On hardware without FEAT_PAuth, pacibsp/autibsp decode as NOPs.

#ifdef __aarch64__

// extern "C" for a predictable .seh_proc symbol name.
//
// Args (AAPCS64): x0 = alt_stack_top (16-aligned), x1 = signum,
//                 x2 = siginfo_t*, x3 = const struct sigaction*,
//                 x4 = ucontext_t*
extern "C" [[gnu::naked]]
void __llvm_libc_deliver_on_alt_stack(void *, int, siginfo_t *,
                                      const struct sigaction *,
                                      ucontext_t *) {
  asm(R"(
      .seh_proc __llvm_libc_deliver_on_alt_stack

      // Sign return address with B-key. Implicit bti c landing pad.
      pacibsp
      .seh_pac_sign_lr

      // Save FP/LR and establish frame pointer chain. The pre-indexed
      // store allocates 16 bytes and the frame record anchors the
      // original stack so the unwinder can walk past the SP switch.
      stp x29, x30, [sp, #-16]!
      .seh_save_fplr_x 16
      mov x29, sp
      .seh_set_fp
      .seh_endprologue

      // Switch SP to alternate signal stack. The caller has already
      // 16-aligned alt_stack_top. Reserve 16 bytes (ABI requires SP
      // to be below any live data).
      mov sp, x0
      sub sp, sp, #16

      // Rearrange args for call_signal_handler(signum, info, action, context).
      // x1 -> x0 (signum), x2 -> x1 (info), x3 -> x2 (action), x4 -> x3 (context).
      // Order matters: move x1 last since x0 is overwritten first.
      mov x0, x1
      mov x1, x2
      mov x2, x3
      mov x3, x4
      bl %c[handler]

      // Restore original stack from frame pointer.
      .seh_startepilogue
      mov sp, x29
      .seh_set_fp
      ldp x29, x30, [sp], #16
      .seh_save_fplr_x 16
      autibsp
      .seh_pac_sign_lr
      .seh_endepilogue
      ret
      .seh_endproc
  )" :: [handler] "i"(call_signal_handler));
}

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {

void deliver_on_alt_stack(void *alt_stack_top, int signum, siginfo_t *info,
                          const struct sigaction *action,
                          ucontext_t *context) {
  __llvm_libc_deliver_on_alt_stack(alt_stack_top, signum, info, action,
                                    context);
}

} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

#endif // __aarch64__
