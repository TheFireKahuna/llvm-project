//===-- Windows implementation of fork -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// fork() on NT-POSIX via RtlCloneUserProcess (ntdll clone wrapper).
//
// Architecture: naked asm wrapper orchestrating four C++ helpers.
//
// The naked wrapper fully controls the execution sequence:
//
//   1. Save ALL callee-saved GPRs, ALL XMM/NEON, FP control to stack
//   2. Call fork_pre()           — fork_prepare (locks, atfork handlers)
//   3. Save TLS pointer (gs:0x58 / [x18,#0x58]) + parent TEB address
//   4. Call fork_do_clone()      — RtlCloneUserProcess, which internally:
//        a. RtlPrepareForProcessCloning (ntdll lock acquisition + drain)
//        b. NtCreateUserProcess (CoW clone)
//        c. RtlCompleteProcessCloning (parent: release locks,
//           child: reset all ntdll locks to unlocked state)
//   5. Restore ALL registers     — before any post-syscall C++ code
//   6. Branch on NTSTATUS:
//        child:  restore TLS, copy parent TEB state, reinit, return 0
//        parent: call fork_parent_post(), return pid or -1
//
// Step 5 is the critical invariant: the register restore happens BETWEEN
// the clone syscall (step 4) and all post-syscall C++ code (step 6).
// No compiler-generated code touches callee-saved registers in that
// window.  The child reads the parent's saved values via CoW stack.
//
// Why this matters — the kernel's clone-mode thread creation may set up
// a fresh CONTEXT rather than copying the parent's register file:
//  - GPRs: fork_do_clone's epilogue only restores registers its body
//    modified; any it didn't touch pass through as kernel-initialized.
//  - XMM6-15: same — compiler saves only what fork_do_clone used.
//  - MXCSR/FCW: never saved by any compiler-generated prologue; if the
//    kernel reinitializes them, all FP math in the child is affected.
//  - TLS (gs:0x58): kernel creates a new TEB with NULL TLS pointer.
//  - TEB TlsSlots: kernel creates a fresh TEB with zeroed inline TLS
//    slots.  The parent's TEB address is saved before clone so the child
//    can copy TlsSlots, TlsExpansionSlots, and LastErrorValue from the
//    parent's COW-mapped TEB.  This matches POSIX: the child is a
//    "replica of the calling thread" with all thread-local data intact.
//
// SEH unwind info (.seh_proc) covers the frame: stack allocation,
// callee-saved GPRs, and callee-saved XMM6-15 / d8-d15.  Volatile
// register saves (XMM0-5, v0-v7) and FP control state are saved for
// fork child correctness only — invisible to the OS unwinder.
//
// RtlCloneUserProcess wraps NtCreateUserProcess in clone mode (no image
// path, no ProcessParameters) with ntdll-internal state protection.
// Prepare acquires 27+ locks (heap, loader, PEB, TLS/FLS, thread pool);
// Complete resets them all in the child.  Without this, any parent thread
// mid-heap-op or mid-DllMain at snapshot time leaves the child with held
// locks and no owning thread — guaranteed deadlock.  Future-proof: new
// ntdll-internal locks are covered automatically.
//
// Boundary: RtlCloneUserProcess owns ntdll lock state; fls_fork_reinit()
// and libc_fork_reinit() own data repair (FLS linked list, libc state).
//
//===----------------------------------------------------------------------===//

#include "src/unistd/fork.h"

#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/OSUtil/windows/nt/nt_process_types.h"
#include "src/__support/OSUtil/windows/nt/nt_peb.h"
#include "src/__support/OSUtil/windows/nt/nt_error.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/OSUtil/windows/process/fork_reinit.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/tls/fls_fork.h"
#include "src/__support/process/windows/child_table.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

// =====================================================================
// ForkResult — lives on the naked wrapper's stack, passed by pointer
// to fork_do_clone (which fills it) and fork_parent_post (which reads).
// =====================================================================
struct ForkResult {
  HANDLE child_process;
  HANDLE child_thread;
};

// =====================================================================
// Helper functions — each has a fixed asm symbol for the naked wrapper.
// All are noinline (can't inline into naked) and used (referenced only
// from asm, invisible to the compiler's DCE).
// =====================================================================

// --- Pre-fork: flush streams, acquire locks, atfork prepare handlers ---
__attribute__((noinline, used))
static void fork_pre() asm("__fork_pre");
static void fork_pre() {
  internal::fork_prepare();
}

// --- Clone: RtlCloneUserProcess (ntdll lock protection + NtCreateUserProcess) ---
//
// RtlCloneUserProcess handles the full clone protocol internally:
//   1. RtlPrepareForProcessCloning — acquires ntdll-internal locks
//      (loader, PEB, TLS, FLS, heap) and drains the thread pool.
//   2. NtCreateUserProcess(nullptr, nullptr) — CoW address space clone.
//   3. RtlCompleteProcessCloning — parent: releases all locks.
//      Child: resets all cloned ntdll critical sections and SRWLocks
//      to their unlocked state (loader lock, PEB lock, heap locks,
//      FLS SRWLock, TLS lock).
//
// This eliminates the risk of inheriting a held ntdll lock from a parent
// thread that was mid-heap-op, mid-DllMain, or mid-FLS-mutation at clone
// time — which would deadlock on the first ntdll call in the child.
//
// Returns STATUS_PROCESS_CLONED in the child, STATUS_SUCCESS in the
// parent.  ForkResult is filled only in the parent.
__attribute__((noinline, used))
static NTSTATUS fork_do_clone(ForkResult *r) asm("__fork_do_clone");
static NTSTATUS fork_do_clone(ForkResult *r) {
  r->child_process = nullptr;
  r->child_thread = nullptr;

  RTL_USER_PROCESS_INFORMATION info = {};
  info.Length = sizeof(info);

  NTSTATUS st = ::RtlCloneUserProcess(
      RTL_CLONE_PROCESS_FLAGS_INHERIT_HANDLES,
      nullptr,  // ProcessSecurityDescriptor
      nullptr,  // ThreadSecurityDescriptor
      nullptr,  // DebugPort
      &info);

  // Parent path: extract handles for waitpid tracking.
  // Child path: STATUS_PROCESS_CLONED — info is not filled.
  if (NT_SUCCESS(st) && st != STATUS_PROCESS_CLONED) {
    r->child_process = info.ProcessHandle;
    r->child_thread = info.ThreadHandle;
  }

  return st;
}

// --- Copy TEB thread-local state from parent's COW TEB to child ---
//
// NtCreateUserProcess (clone mode) gives the child a new TEB with zeroed
// TlsSlots, NULL TlsExpansionSlots, and default LastErrorValue.  POSIX
// requires the child to be a "replica of the calling thread" — all
// thread-local data must survive fork.
//
// The asm wrapper already restores ThreadLocalStoragePointer (gs:0x58),
// which covers C++ thread_local / __declspec(thread) variables whose
// data blocks reside in COW memory.  This function covers the remaining
// TEB-resident state:
//
//   TlsSlots[0..63]   — inline Win32 TLS (TlsSetValue) + our teb_tls_set
//   TlsExpansionSlots  — Win32 TLS indices >= 64 (COW-valid pointer)
//   LastErrorValue     — Win32 GetLastError / NT RtlGetLastWin32Error
//
// The parent's TEB address (saved before clone) points to COW-mapped
// pages in the child's address space — readable, no handle needed.
static void copy_parent_teb_state(void *parent_teb_ptr) {
  auto *parent = static_cast<TEB *>(parent_teb_ptr);
  TEB *child = NtCurrentTeb();

  // Inline TLS slots: 64 PVOID entries at TEB+0x1480.
  // Covers both Win32 TlsSetValue and our lock-free teb_tls_set.
  // Without this copy, every subsystem's per-thread TLS (VEH reentry
  // guard, fault guard, SlabPool caches, thread lifecycle, wait slots,
  // etc.) would be NULL until reinit re-establishes them.
  for (unsigned i = 0; i < TLS_MINIMUM_AVAILABLE; ++i)
    child->TlsSlots[i] = parent->TlsSlots[i];

  // Expansion slots: pointer at TEB+0x1780 to a heap-allocated array
  // for TLS indices >= 64.  The array itself is in COW memory, so
  // copying the pointer gives the child correct read access.  If either
  // process later writes to a slot, the COW page splits independently.
  child->TlsExpansionSlots = parent->TlsExpansionSlots;

  // Last error value: ULONG at TEB+0x68.
  child->LastErrorValue = parent->LastErrorValue;
}

// --- Child post-processing (runs AFTER register restore) ---
//
// By the time we get here, RtlCloneUserProcess has already called
// RtlCompleteProcessCloning(TRUE) internally, which reset all ntdll-
// internal locks (loader, PEB, heap, TLS, FLS SRWLocks and critical
// sections) to their unlocked state.  The heap, loader, and all ntdll
// primitives are safe to use.
__attribute__((noinline, used))
static void fork_child_post(void *parent_teb) asm("__fork_child_post");
static void fork_child_post(void *parent_teb) {
  // Copy TEB TLS state from the parent's COW TEB to the child's fresh
  // TEB.  Done first so that TLS slots are correct before any code
  // (including ShutdownInProgress access) could trigger an exception
  // that consults TLS-resident state via the VEH reentry/fault guards.
  copy_parent_teb_state(parent_teb);

  // Repair ntdll FLS data structures: clean the stale FLS linked list
  // (entries from parent threads that don't exist in the child), reuse
  // the parent's COW FLS_DATA block with its slot values, and set the
  // child's TEB->FlsData.  The FLS SRWLock was already reset by
  // RtlCompleteProcessCloning(TRUE) inside RtlCloneUserProcess.
  internal::windows::fls_fork_reinit(parent_teb);

  // The cloned PEB_LDR_DATA has ShutdownInProgress=TRUE, which makes
  // ntdll think the process is exiting.  Clear it before anything else.
  NtCurrentPeb()->Ldr->ShutdownInProgress = FALSE;

  // Full reinit chain: reset subsystem state (locks, reactor, ALPC,
  // thread registry, etc.) then invoke pthread_atfork child handlers.
  internal::fork_child();
}

// --- Parent post-processing (runs AFTER register restore) ---
//
// Returns the kernel-convention intptr_t: pid on success, -errno on
// failure. The POSIX LLVM_LIBC_FUNCTION(fork) wrapper converts to
// -1/libc_errno; the SYS_fork dispatch case consumes -errno directly.
__attribute__((noinline, used))
static intptr_t fork_parent_post(NTSTATUS status,
                                 ForkResult *r) asm("__fork_parent_post");
static intptr_t fork_parent_post(NTSTATUS status, ForkResult *r) {
  // RtlCloneUserProcess already called RtlCompleteProcessCloning(FALSE)
  // internally before returning to us — ntdll locks are released.

  if (!NT_SUCCESS(status)) {
    internal::fork_parent(); // Unwind prepare handlers even on failure.
    return -static_cast<intptr_t>(windows_util::ntstatus_to_errno(status));
  }

  // Invoke pthread_atfork parent handlers, release critical locks.
  internal::fork_parent();

  // Get the child's PID.
  PROCESS_BASIC_INFORMATION pbi = {};
  ULONG rlen = 0;
  ::NtQueryInformationProcess(r->child_process, ProcessBasicInformation,
                              &pbi, sizeof(pbi), &rlen);
  pid_t child_pid = static_cast<pid_t>(pbi.UniqueProcessId);

  // Close the child thread handle — we don't need it.
  ::NtClose(r->child_thread);

  // Track the child for waitpid/SIGCHLD.
  pid_t child_pgid = process::get_self_pgid();
  int track_err = process::track_child(r->child_process, child_pid,
                                       child_pgid);
  if (track_err) {
    // Child is already running — we can't un-fork it.  Close the handle
    // to avoid leaking it in the parent.
    ::NtClose(r->child_process);
  }

  return static_cast<intptr_t>(child_pid);
}

// =====================================================================
// Naked asm wrapper — the public fork() entry point.
//
// Orchestrates the four helpers above with full register save/restore
// bracketing the clone syscall.  The restore (step 5 in the header
// comment) runs BEFORE any post-syscall C++ code, closing the
// callee-saved register gap entirely.
// =====================================================================

#if defined(__x86_64__)

// x64 stack frame layout — SysV AMD64 caller convention (88 bytes, RSP
// 16-byte aligned after alloc):
//
//   Offset  Size  Content
//   ------  ----  -------
//     0       8   Callee-saved GPR: rbx
//     8       8   Callee-saved GPR: rbp
//    16       8   Callee-saved GPR: r12
//    24       8   Callee-saved GPR: r13
//    32       8   Callee-saved GPR: r14
//    40       8   Callee-saved GPR: r15
//    48       8   Saved TLS pointer (gs:0x58)
//    56       8   Saved parent TEB address (gs:0x30)
//    64       8   ForkResult.child_process
//    72       8   ForkResult.child_thread
//    80       8   Alignment padding
//   ------  ----
//    88     total
//
// Entry RSP = 8 mod 16 (return address pushed by caller).
// After sub $88: RSP = (8 - 88) mod 16 = 0 mod 16.  Correct for `call`.
//
// Why this shrinks versus the MS x64 version:
//   * No rdi/rsi save — they are caller-saved (volatile) under SysV.
//   * No XMM save/restore — all XMM0-15 are volatile under SysV; the
//     callers we invoke (__fork_pre / __fork_do_clone / __fork_child_post
//     / __fork_parent_post) do not need them preserved.  The child-
//     correctness argument for XMM6-15 no longer applies because the
//     caller of __llvm_libc_fork does not expect them preserved either.
//   * No MXCSR / FCW save — volatile in SysV as well.
//   * No shadow space in this frame — every direct call this wrapper
//     issues is to a SysV helper.  The MS x64 ABI boundary into ntdll
//     (RtlCloneUserProcess) is crossed inside __fork_do_clone's own
//     frame, where clang allocates shadow space automatically.
//
// Alignment derivation:
//   entry RSP % 16 == 8
//   required content: 6*8 GPR + 8 TLS + 8 parentTEB + 16 ForkResult = 80
//   smallest N >= 80 with N % 16 == 8 → N = 88
//   post-sub RSP % 16 == (8 - 88) % 16 == 0.  OK.

// Engine: naked-asm clone orchestrator. Exported as the extern "C"
// symbol __llvm_libc_sys_fork so both the POSIX LLVM_LIBC_FUNCTION(fork)
// wrapper below AND the SYS_fork dispatch case can reach it by name.
// Kernel convention: >0 pid (parent), 0 (child), -errno (parent error).
extern "C" [[gnu::naked]] intptr_t __llvm_libc_sys_fork(void) {
  asm(R"(
      .seh_proc __llvm_libc_sys_fork
      subq   $88, %rsp
      .seh_stackalloc 88

      # ===== Save callee-saved GPRs (SysV: rbx rbp r12-r15) =====
      movq   %rbx,  0(%rsp)
      .seh_savereg %rbx, 0
      movq   %rbp,  8(%rsp)
      .seh_savereg %rbp, 8
      movq   %r12, 16(%rsp)
      .seh_savereg %r12, 16
      movq   %r13, 24(%rsp)
      .seh_savereg %r13, 24
      movq   %r14, 32(%rsp)
      .seh_savereg %r14, 32
      movq   %r15, 40(%rsp)
      .seh_savereg %r15, 40

      .seh_endprologue

      # ===== Step 2: Pre-fork (locks, stream flush, atfork prepare) =====
      call   __fork_pre

      # ===== Step 3: Save TLS pointer + parent TEB address =====
      # ThreadLocalStoragePointer — restored in child asm path (gs:0x58).
      movq   %gs:0x58, %rax
      movq   %rax, 48(%rsp)
      # Parent TEB self-pointer — passed to fork_child_post for
      # TlsSlots/TlsExpansionSlots/LastErrorValue copy from COW TEB.
      movq   %gs:0x30, %rax
      movq   %rax, 56(%rsp)

      # ===== Step 4: Clone via NtCreateUserProcess =====
      # SysV arg1 = rdi.  Helper is SysV; clang bridges to MS x64 ntdll
      # callee inside __fork_do_clone's own frame (with shadow space).
      leaq   64(%rsp), %rdi       # arg1 = &ForkResult
      call   __fork_do_clone
      # rax = NTSTATUS (volatile — survives the restore below)

      # ===== Step 5: Restore callee-saved GPRs =====
      # Critical for child (kernel may not preserve CONTEXT beyond what
      # the clone-mode NtCreateUserProcess reinitialises).
      # Harmless for parent (values already correct in register file).
      # rax is volatile and NOT touched by the restore.
      movq    0(%rsp), %rbx
      movq    8(%rsp), %rbp
      movq   16(%rsp), %r12
      movq   24(%rsp), %r13
      movq   32(%rsp), %r14
      movq   40(%rsp), %r15

      # ===== Step 6: Branch child vs parent =====
      # rax = NTSTATUS from fork_do_clone (untouched by restore).
      cmpl   $0x129, %eax          # STATUS_PROCESS_CLONED
      je     1f

      # --- Parent path ---
      # SysV: arg1 = rdi, arg2 = rsi.
      movl   %eax, %edi           # arg1 = NTSTATUS
      leaq   64(%rsp), %rsi       # arg2 = &ForkResult
      call   __fork_parent_post
      # rax = pid_t (>0 success, -1 error)
      jmp    2f

  1:  # --- Child path ---
      # Restore ThreadLocalStoragePointer (gs:0x58) from frame.
      # The kernel created a new TEB but did not copy this field.
      movq   48(%rsp), %rax
      movq   %rax, %gs:0x58

      # Pass parent TEB address for TlsSlots + expansion + error copy.
      # SysV: arg1 = rdi.
      movq   56(%rsp), %rdi       # arg1 = parent_teb
      # TEB state copy + PEB fix + full reinit + atfork child handlers.
      call   __fork_child_post
      xorl   %eax, %eax           # return 0

  2:  # --- Epilogue ---
      addq   $88, %rsp
      retq
      .seh_endproc
  )");
}

#elif defined(__aarch64__)

// AArch64 stack frame layout (400 bytes, SP 16-byte aligned):
//
//   Offset  Size  Content
//   ------  ----  -------
//     0      80   Callee-saved GPRs: x19-x28 (5 stp pairs)
//    80      16   fp (x29) + lr (x30)
//    96     256   v0-v15 (16 NEON regs x 16 bytes, stp q-form)
//   352       4   FPCR
//   356       4   FPSR
//   360       8   Saved TLS pointer ([x18, #0x58])
//   368       8   Saved parent TEB address ([x18, #0x30])
//   376       8   ForkResult.child_process
//   384       8   ForkResult.child_thread
//   392       8   (padding to 16-byte alignment)
//   ------  ----
//   400     total

extern "C" [[gnu::naked]] intptr_t __llvm_libc_sys_fork(void) {
  asm(R"(
      .seh_proc __llvm_libc_sys_fork
      sub    sp, sp, #400
      .seh_stackalloc 400

      // ===== Save callee-saved GPRs + fp/lr =====
      stp    x19, x20, [sp, #0]
      .seh_save_regp x19, 0
      stp    x21, x22, [sp, #16]
      .seh_save_regp x21, 16
      stp    x23, x24, [sp, #32]
      .seh_save_regp x23, 32
      stp    x25, x26, [sp, #48]
      .seh_save_regp x25, 48
      stp    x27, x28, [sp, #64]
      .seh_save_regp x27, 64
      stp    x29, x30, [sp, #80]
      .seh_save_fplr 80

      // ===== Save ALL NEON v0-v15 (128-bit each) =====
      // v0-v7 are volatile — saved for fork child correctness only,
      // not described to the SEH unwinder.
      stp    q0,  q1,  [sp, #96]
      stp    q2,  q3,  [sp, #128]
      stp    q4,  q5,  [sp, #160]
      stp    q6,  q7,  [sp, #192]
      // v8-v15 are callee-saved — full 128-bit q-form saves described
      // to the SEH unwinder via .seh_save_any_reg_p.
      stp    q8,  q9,  [sp, #224]
      .seh_save_any_reg_p q8, 224
      stp    q10, q11, [sp, #256]
      .seh_save_any_reg_p q10, 256
      stp    q12, q13, [sp, #288]
      .seh_save_any_reg_p q12, 288
      stp    q14, q15, [sp, #320]
      .seh_save_any_reg_p q14, 320

      // ===== Save FP control/status =====
      // Not described to SEH — runtime FP state, not frame linkage.
      mrs    x9, fpcr
      mrs    x10, fpsr
      str    w9, [sp, #352]
      str    w10, [sp, #356]

      .seh_endprologue

      // ===== Step 2: Pre-fork =====
      bl     __fork_pre

      // ===== Step 3: Save TLS pointer + parent TEB address =====
      // ThreadLocalStoragePointer — restored in child asm path.
      ldr    x9, [x18, #0x58]
      str    x9, [sp, #360]
      // Parent TEB self-pointer — passed to fork_child_post for
      // TlsSlots/TlsExpansionSlots/LastErrorValue copy from COW TEB.
      // x18 = TEB base (platform register), equals TEB.NtTib.Self.
      ldr    x9, [x18, #0x30]
      str    x9, [sp, #368]

      // ===== Step 4: Clone =====
      add    x0, sp, #376           // arg1 = &ForkResult
      bl     __fork_do_clone
      // x0 = NTSTATUS (volatile — survives restore)

      // ===== Step 5: Restore ALL registers =====
      ldp    x19, x20, [sp, #0]
      ldp    x21, x22, [sp, #16]
      ldp    x23, x24, [sp, #32]
      ldp    x25, x26, [sp, #48]
      ldp    x27, x28, [sp, #64]
      ldp    x29, x30, [sp, #80]

      ldp    q0,  q1,  [sp, #96]
      ldp    q2,  q3,  [sp, #128]
      ldp    q4,  q5,  [sp, #160]
      ldp    q6,  q7,  [sp, #192]
      ldp    q8,  q9,  [sp, #224]
      ldp    q10, q11, [sp, #256]
      ldp    q12, q13, [sp, #288]
      ldp    q14, q15, [sp, #320]

      ldr    w9, [sp, #352]
      ldr    w10, [sp, #356]
      msr    fpcr, x9
      msr    fpsr, x10

      // ===== Step 6: Branch child vs parent =====
      cmp    w0, #0x129              // STATUS_PROCESS_CLONED
      b.eq   1f

      // --- Parent path ---
      // x0 = NTSTATUS already in place (arg1)
      add    x1, sp, #376           // arg2 = &ForkResult
      bl     __fork_parent_post
      // x0 = pid_t
      b      2f

  1:  // --- Child path ---
      // Restore ThreadLocalStoragePointer ([x18, #0x58]) from frame.
      ldr    x9, [sp, #360]
      str    x9, [x18, #0x58]

      // Pass parent TEB address for TlsSlots + expansion + error copy.
      ldr    x0, [sp, #368]         // arg1 = parent_teb
      // TEB state copy + PEB fix + full reinit + atfork child handlers.
      bl     __fork_child_post
      mov    x0, #0                 // return 0

  2:  // --- Epilogue ---
      add    sp, sp, #400
      ret
      .seh_endproc
  )");
}

#else
#error "fork: unsupported architecture"
#endif

// =====================================================================
// C++ surface: internal::sys_fork (kernel convention) + POSIX fork().
// =====================================================================
//
// internal::sys_fork() is the C++-callable alias for the naked-asm
// __llvm_libc_sys_fork symbol above. Used by:
//   - the POSIX LLVM_LIBC_FUNCTION(pid_t, fork) wrapper below, and
//   - the SYS_fork / SYS_vfork cases in osutil/windows/syscall.h
//     (which consume -errno directly — that's why we route through
//     this alias rather than re-invoking the POSIX entrypoint).
namespace internal {
intptr_t sys_fork(void) asm("__llvm_libc_sys_fork");
} // namespace internal

// Thin POSIX wrapper: converts kernel convention (-errno) to POSIX
// convention (-1 + libc_errno). Not naked, no SEH frame of its own —
// the engine owns all frame discipline.
LLVM_LIBC_FUNCTION(pid_t, fork, (void)) {
  intptr_t ret = internal::sys_fork();
  if (ret < 0) {
    libc_errno = static_cast<int>(-ret);
    return -1;
  }
  return static_cast<pid_t>(ret);
}

} // namespace LIBC_NAMESPACE_DECL
