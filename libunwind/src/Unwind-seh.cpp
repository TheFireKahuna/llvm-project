//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Implements SEH-based Itanium C++ exceptions.
//
//===----------------------------------------------------------------------===//

#include "config.h"

#if defined(_LIBUNWIND_SUPPORT_SEH_UNWIND)

#include <unwind.h>

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include <windef.h>
#include <excpt.h>
#include <winnt.h>
#include <ntstatus.h>

#include "libunwind_ext.h"
#include "UnwindCursor.hpp"

using namespace libunwind;

#define STATUS_USER_DEFINED (1u << 29)

#define STATUS_GCC_MAGIC  (('G' << 16) | ('C' << 8) | 'C')

#define MAKE_CUSTOM_STATUS(s, c) \
  ((NTSTATUS)(((s) << 30) | STATUS_USER_DEFINED | (c)))
#define MAKE_GCC_EXCEPTION(c) \
  MAKE_CUSTOM_STATUS(STATUS_SEVERITY_SUCCESS, STATUS_GCC_MAGIC | ((c) << 24))

/// SEH exception raised by libunwind when the program calls
/// \c _Unwind_RaiseException.
#define STATUS_GCC_THROW MAKE_GCC_EXCEPTION(0) // 0x20474343
/// SEH exception raised by libunwind to initiate phase 2 of exception
/// handling.
#define STATUS_GCC_UNWIND MAKE_GCC_EXCEPTION(1) // 0x21474343

static int __unw_init_seh(unw_cursor_t *cursor, CONTEXT *ctx);
static DISPATCHER_CONTEXT *__unw_seh_get_disp_ctx(unw_cursor_t *cursor);
static void __unw_seh_set_disp_ctx(unw_cursor_t *cursor,
                                   DISPATCHER_CONTEXT *disp);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-anonymous-struct"
// Local redefinition of this type; mingw-w64 headers lack the
// DISPATCHER_CONTEXT_NONVOLREG_ARM64 type as of May 2025, so locally redefine
// it and use that definition, to avoid needing to test/guess whether the real
// type is available of not.
union LOCAL_DISPATCHER_CONTEXT_NONVOLREG_ARM64 {
  BYTE Buffer[11 * sizeof(DWORD64) + 8 * sizeof(double)];

  struct {
    DWORD64 GpNvRegs[11];
    double FpNvRegs[8];
  };
};

// Custom data type definition; this type is not defined in WinSDK.
union LOCAL_DISPATCHER_CONTEXT_NONVOLREG_ARM {
  BYTE Buffer[8 * sizeof(DWORD) + 8 * sizeof(double)];

  struct {
    DWORD GpNvRegs[8];
    double FpNvRegs[8];
  };
};
#pragma clang diagnostic pop

/// Returns the LSDA of the frame of \p disp. On Windows Itanium this unwinder
/// calls a frame's cleanups and chooses the handler it lands in, and the
/// frame's handler data leads with a table of its EH scopes: a version byte,
/// the table's size, the number of scopes, the scopes and the call sites, as
/// ULEB128 numbers but for a selector. A scope gives the number of the scope
/// around it, counted from one and lower than its own, or zero for none, then
/// the number of its handlers and, for a cleanup, which has none, the
/// cleanup's offset in the function, or for each handler its selector and its
/// pad's offset. A call site gives its start and its scope. \p scopes receives
/// the table past its size; it ends at the LSDA.
static uintptr_t getLSDA(const DISPATCHER_CONTEXT *disp,
                         uintptr_t *scopes = nullptr) {
  uintptr_t data = (uintptr_t)disp->HandlerData;
#if defined(_WIN32_ITANIUM)
  if (!data)
    return 0;
  if (*(const uint8_t *)data++ != 2)
    _LIBUNWIND_ABORT("unknown version of the scope table");
  uintptr_t size = (uintptr_t)LocalAddressSpace::getULEB128(data, UINTPTR_MAX);
  if (scopes)
    *scopes = data;
  data += size;
#endif
  return data;
}

#if defined(_WIN32_ITANIUM)
/// Returns the end of the call-site table of the LSDA at \p p, with \p p moved
/// to the table's first record and \p encoding set to that of its fields.
static uintptr_t getCallSites(uintptr_t &p, uint8_t &encoding) {
  LocalAddressSpace &as = LocalAddressSpace::sThisAddressSpace;
  encoding = as.get8(p++);
  if (encoding != DW_EH_PE_omit)
    as.getEncodedP(p, UINTPTR_MAX, encoding);
  encoding = as.get8(p++);
  if (encoding != DW_EH_PE_omit)
    as.getULEB128(p, UINTPTR_MAX);
  encoding = as.get8(p++);
  uintptr_t size = (uintptr_t)as.getULEB128(p, UINTPTR_MAX);
  return p + size;
}

/// The executable section of the image at \p base that holds \p address, as
/// its start and size, or an empty range.
struct CodeRange {
  uintptr_t start = 0, size = 0;
  bool contains(uintptr_t address) const { return address - start < size; }
};
static CodeRange getCodeRange(uintptr_t base, uintptr_t address) {
  auto *nt =
      (const IMAGE_NT_HEADERS *)(base +
                                 ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
  const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i != nt->FileHeader.NumberOfSections; ++i, ++section)
    if (address - base - section->VirtualAddress < section->Misc.VirtualSize)
      return section->Characteristics & IMAGE_SCN_MEM_EXECUTE
                 ? CodeRange{base + section->VirtualAddress,
                             section->Misc.VirtualSize}
                 : CodeRange{};
  return {};
}

/// Calls each cleanup of the null-terminated \p cleanups in turn with \p exc
/// and \p frame, the frame's callee-saved registers loaded from \p ctx, the
/// context the unwinder holds for the frame at the call the unwind passes. It
/// saves its own once, and stores nothing back: a cleanup hands on nothing in a
/// register, and saves none, so each is given the frame's afresh.
extern "C" void __unw_seh_call_cleanups(const uintptr_t *cleanups, void *exc,
                                        void *frame, const CONTEXT *ctx);

/// The handler of the routine's frame. A cleanup does not unwind, and has no
/// handler of its own: a search that leaves one ends here. A C++ exception has
/// its raise resumed, so the thrower terminates. Any other goes to the
/// unhandled-exception filter with its own record, and the process ends. A
/// longjmp or a forced unwind that leaves the cleanup is no search.
extern "C" EXCEPTION_DISPOSITION
__unw_seh_cleanup_handler(PEXCEPTION_RECORD ms_exc, PVOID, PCONTEXT ms_ctx,
                          DISPATCHER_CONTEXT *) {
  bool raised = ms_exc->ExceptionCode == STATUS_GCC_THROW;
  if (IS_UNWINDING(ms_exc->ExceptionFlags) ||
      (raised && ms_exc->NumberParameters > 1))
    return ExceptionContinueSearch;
  EXCEPTION_POINTERS info = {ms_exc, ms_ctx};
  if (!raised &&
      UnhandledExceptionFilter(&info) != EXCEPTION_CONTINUE_EXECUTION)
    TerminateProcess(GetCurrentProcess(), ms_exc->ExceptionCode);
  return ExceptionContinueExecution;
}
#if defined(__x86_64__)
static_assert(offsetof(CONTEXT, Rbx) == 0x90 && offsetof(CONTEXT, Rbp) == 0xa0 &&
                  offsetof(CONTEXT, Rsi) == 0xa8 &&
                  offsetof(CONTEXT, Rdi) == 0xb0 &&
                  offsetof(CONTEXT, R12) == 0xd8 &&
                  offsetof(CONTEXT, R15) == 0xf0 &&
                  offsetof(CONTEXT, Xmm6) == 0x200 &&
                  offsetof(CONTEXT, Xmm15) == 0x290,
              "the routine's offsets in CONTEXT");
asm(R"(
  .text
  .def __unw_seh_call_cleanups
  .scl 2
  .type 32
  .endef
  .globl __unw_seh_call_cleanups
  .p2align 4
__unw_seh_call_cleanups:
  .seh_proc __unw_seh_call_cleanups
  .seh_handler __unw_seh_cleanup_handler, @except
  pushq %rbp
  .seh_pushreg %rbp
  pushq %rbx
  .seh_pushreg %rbx
  pushq %rdi
  .seh_pushreg %rdi
  pushq %rsi
  .seh_pushreg %rsi
  pushq %r12
  .seh_pushreg %r12
  pushq %r13
  .seh_pushreg %r13
  pushq %r14
  .seh_pushreg %r14
  pushq %r15
  .seh_pushreg %r15
  subq $232, %rsp
  .seh_stackalloc 232
  movaps %xmm6, 32(%rsp)
  .seh_savexmm %xmm6, 32
  movaps %xmm7, 48(%rsp)
  .seh_savexmm %xmm7, 48
  movaps %xmm8, 64(%rsp)
  .seh_savexmm %xmm8, 64
  movaps %xmm9, 80(%rsp)
  .seh_savexmm %xmm9, 80
  movaps %xmm10, 96(%rsp)
  .seh_savexmm %xmm10, 96
  movaps %xmm11, 112(%rsp)
  .seh_savexmm %xmm11, 112
  movaps %xmm12, 128(%rsp)
  .seh_savexmm %xmm12, 128
  movaps %xmm13, 144(%rsp)
  .seh_savexmm %xmm13, 144
  movaps %xmm14, 160(%rsp)
  .seh_savexmm %xmm14, 160
  movaps %xmm15, 176(%rsp)
  .seh_savexmm %xmm15, 176
  .seh_endprologue
  movq %rcx, 192(%rsp)
  movq %rdx, 200(%rsp)
  movq %r8, 208(%rsp)
  movq %r9, 216(%rsp)
1:
  movq 192(%rsp), %rax
  movq (%rax), %rax
  testq %rax, %rax
  jz 2f
  addq $8, 192(%rsp)
  movq 200(%rsp), %rcx
  movq 208(%rsp), %rdx
  movq 216(%rsp), %r9
  movq 0x90(%r9), %rbx
  movq 0xa0(%r9), %rbp
  movq 0xa8(%r9), %rsi
  movq 0xb0(%r9), %rdi
  movq 0xd8(%r9), %r12
  movq 0xe0(%r9), %r13
  movq 0xe8(%r9), %r14
  movq 0xf0(%r9), %r15
  movaps 0x200(%r9), %xmm6
  movaps 0x210(%r9), %xmm7
  movaps 0x220(%r9), %xmm8
  movaps 0x230(%r9), %xmm9
  movaps 0x240(%r9), %xmm10
  movaps 0x250(%r9), %xmm11
  movaps 0x260(%r9), %xmm12
  movaps 0x270(%r9), %xmm13
  movaps 0x280(%r9), %xmm14
  movaps 0x290(%r9), %xmm15
  callq *%rax
  jmp 1b
2:
  movaps 32(%rsp), %xmm6
  movaps 48(%rsp), %xmm7
  movaps 64(%rsp), %xmm8
  movaps 80(%rsp), %xmm9
  movaps 96(%rsp), %xmm10
  movaps 112(%rsp), %xmm11
  movaps 128(%rsp), %xmm12
  movaps 144(%rsp), %xmm13
  movaps 160(%rsp), %xmm14
  movaps 176(%rsp), %xmm15
  .seh_startepilogue
  addq $232, %rsp
  popq %r15
  popq %r14
  popq %r13
  popq %r12
  popq %rsi
  popq %rdi
  popq %rbx
  popq %rbp
  .seh_endepilogue
  retq
  .seh_endproc
)");
#elif defined(__aarch64__)
static_assert(offsetof(CONTEXT, X19) == 0xa0 &&
                  offsetof(CONTEXT, X27) == 0xe0 &&
                  offsetof(CONTEXT, Fp) == 0xf0 &&
                  offsetof(CONTEXT, V[8]) == 0x190 &&
                  offsetof(CONTEXT, V[15]) == 0x200,
              "the routine's offsets in CONTEXT");
asm(R"(
  .text
  .def __unw_seh_call_cleanups
  .scl 2
  .type 32
  .endef
  .globl __unw_seh_call_cleanups
  .p2align 2
__unw_seh_call_cleanups:
  .seh_proc __unw_seh_call_cleanups
  .seh_handler __unw_seh_cleanup_handler, @except
  stp x29, x30, [sp, #-192]!
  .seh_save_fplr_x 192
  stp x19, x20, [sp, #16]
  .seh_save_regp x19, 16
  stp x21, x22, [sp, #32]
  .seh_save_regp x21, 32
  stp x23, x24, [sp, #48]
  .seh_save_regp x23, 48
  stp x25, x26, [sp, #64]
  .seh_save_regp x25, 64
  stp x27, x28, [sp, #80]
  .seh_save_regp x27, 80
  stp d8, d9, [sp, #96]
  .seh_save_fregp d8, 96
  stp d10, d11, [sp, #112]
  .seh_save_fregp d10, 112
  stp d12, d13, [sp, #128]
  .seh_save_fregp d12, 128
  stp d14, d15, [sp, #144]
  .seh_save_fregp d14, 144
  .seh_endprologue
  stp x0, x1, [sp, #160]
  stp x2, x3, [sp, #176]
1:
  ldr x9, [sp, #160]
  ldr x16, [x9], #8
  cbz x16, 2f
  str x9, [sp, #160]
  ldp x0, x1, [sp, #168]
  ldr x3, [sp, #184]
  ldp x19, x20, [x3, #0xa0]
  ldp x21, x22, [x3, #0xb0]
  ldp x23, x24, [x3, #0xc0]
  ldp x25, x26, [x3, #0xd0]
  ldp x27, x28, [x3, #0xe0]
  ldr x29, [x3, #0xf0]
  ldr d8, [x3, #0x190]
  ldr d9, [x3, #0x1a0]
  ldr d10, [x3, #0x1b0]
  ldr d11, [x3, #0x1c0]
  ldr d12, [x3, #0x1d0]
  ldr d13, [x3, #0x1e0]
  ldr d14, [x3, #0x1f0]
  ldr d15, [x3, #0x200]
  blr x16
  b 1b
2:
  .seh_startepilogue
  ldp d14, d15, [sp, #144]
  .seh_save_fregp d14, 144
  ldp d12, d13, [sp, #128]
  .seh_save_fregp d12, 128
  ldp d10, d11, [sp, #112]
  .seh_save_fregp d10, 112
  ldp d8, d9, [sp, #96]
  .seh_save_fregp d8, 96
  ldp x27, x28, [sp, #80]
  .seh_save_regp x27, 80
  ldp x25, x26, [sp, #64]
  .seh_save_regp x25, 64
  ldp x23, x24, [sp, #48]
  .seh_save_regp x23, 48
  ldp x21, x22, [sp, #32]
  .seh_save_regp x21, 32
  ldp x19, x20, [sp, #16]
  .seh_save_regp x19, 16
  ldp x29, x30, [sp], #192
  .seh_save_fplr_x 192
  .seh_endepilogue
  ret
  .seh_endproc
)");
#endif

/// Calls the cleanups of the call site an unwind is at in the frame of
/// \p disp, at the address \p pc, innermost first. An unwind that passes the frame, for which
/// \p selector is null, calls them all. One that lands in the frame calls
/// those inside the innermost try block with a handler for \p selector, the
/// personality routine's choice, and returns that handler's pad; it returns 0
/// where no try block of the site has one. The image's read-only unwind data
/// names the cleanups and the pads. A cleanup is no function: it has no type
/// to check and is no valid indirect-call target, so the routine's call is
/// trusted as the system's call of a language handler is, once the cleanup is
/// known to be code of the section that holds the frame's function. The run of
/// cleanups up to the pad, or all of them, goes to the routine at once.
static uintptr_t
callCleanups(PEXCEPTION_RECORD ms_exc, PVOID frame, DISPATCHER_CONTEXT *disp,
             uintptr_t pc, const int64_t *selector = nullptr) {
  // A longjmp or a forced unwind that leaves a cleanup collides with the
  // unwind that called it, and the handler of the cleanup's function is called
  // again. An exception cannot leave a cleanup. No cleanup is entered again.
  if (ms_exc->ExceptionFlags & EXCEPTION_COLLIDED_UNWIND)
    return 0;

  LocalAddressSpace &as = LocalAddressSpace::sThisAddressSpace;
  uintptr_t scopes;
  uintptr_t p = getLSDA(disp, &scopes);
  const uintptr_t scopesEnd = p;
  const uintptr_t start = disp->ImageBase + disp->FunctionEntry->BeginAddress;
  // A return address belongs to the call site ahead of it. AArch64's
  // dispatcher says whether the frame's address is one.
#if defined(__aarch64__)
  const uintptr_t ip = pc - disp->ControlPcIsUnwound - start;
#else
  const uintptr_t ip = pc - 1 - start;
#endif

  // Find the call site in the LSDA: the scope table's sites have its starts.
  uint8_t encoding;
  const uintptr_t sitesEnd = getCallSites(p, encoding);
  uintptr_t site = UINTPTR_MAX;
  while (p < sitesEnd && site == UINTPTR_MAX) {
    uintptr_t siteStart = as.getEncodedP(p, sitesEnd, encoding);
    uintptr_t length = as.getEncodedP(p, sitesEnd, encoding);
    as.getEncodedP(p, sitesEnd, encoding); // The landing pad.
    as.getULEB128(p, sitesEnd);            // The action.
    if (ip - siteStart < length)
      site = siteStart;
  }

  // Reads the scope at \p p: the scope around it and its handlers' number,
  // leaving \p p at the cleanup's offset or the first handler.
  auto readScope = [&](uintptr_t &p, uint64_t &outer) {
    outer = as.getULEB128(p, scopesEnd);
    return as.getULEB128(p, scopesEnd);
  };
  // Returns where the scope numbered \p n begins; the call sites follow the
  // last scope.
  auto seek = [&](uint64_t n) {
    uintptr_t q = scopes;
    for (uint64_t i = 1, outer; i != n; ++i) {
      uint64_t handlers = readScope(q, outer);
      if (!handlers)
        as.getULEB128(q, scopesEnd);
      for (; handlers; --handlers) {
        as.getSLEB128(q, scopesEnd);
        as.getULEB128(q, scopesEnd);
      }
    }
    return q;
  };
  const uint64_t count = as.getULEB128(scopes, scopesEnd);
  p = seek(count + 1);
  uint64_t scope = 0;
  while (p < scopesEnd && !scope) {
    uintptr_t siteStart = (uintptr_t)as.getULEB128(p, scopesEnd);
    uint64_t siteScope = as.getULEB128(p, scopesEnd);
    if (siteStart == site)
      scope = siteScope;
  }

#if defined(__aarch64__)
  // A cleanup reaches its function's frame by the frame pointer.
  frame = (PVOID)disp->ContextRecord->Fp;
#endif
  // A function's cleanups and pads are code of the section that holds it.
  const CodeRange code = getCodeRange(disp->ImageBase, start);
  uintptr_t cleanups[17];
  unsigned pending = 0;
  auto callPending = [&] {
    if (!pending)
      return;
    cleanups[pending] = 0;
    pending = 0;
    __unw_seh_call_cleanups(cleanups, ms_exc, frame, disp->ContextRecord);
  };
  while (scope) {
    if (scope > count)
      _LIBUNWIND_ABORT("scope table names a scope it does not hold");
    // A scope is found by reading those ahead of it.
    p = seek(scope);
    uint64_t outer, handlers = readScope(p, outer);
    // The scope around a scope has a lower number, so the walk ends.
    if (outer >= scope)
      _LIBUNWIND_ABORT("scope table is out of order");
    if (!handlers) {
      uintptr_t cleanup = start + (uintptr_t)as.getULEB128(p, scopesEnd);
      if (!code.contains(cleanup))
        _LIBUNWIND_ABORT("cleanup outside the code of its function");
      _LIBUNWIND_TRACE_UNWINDING("callCleanups() calling cleanup %p(%p, %p)",
                                 (void *)cleanup, (void *)ms_exc, frame);
      if (pending == 16)
        callPending();
      cleanups[pending++] = cleanup;
    }
    for (; handlers; --handlers) {
      int64_t handled = as.getSLEB128(p, scopesEnd);
      uintptr_t pad = start + (uintptr_t)as.getULEB128(p, scopesEnd);
      if (!selector || handled != *selector)
        continue;
      if (!code.contains(pad))
        _LIBUNWIND_ABORT("handler outside the code of its function");
      callPending();
      return pad;
    }
    scope = outer;
  }
  callPending();
  return 0;
}
#endif

/// Common implementation of SEH-style handler functions used by Itanium-
/// style frames.  Depending on how and why it was called, it may do one of:
///  a) Delegate to the given Itanium-style personality function; or
///  b) Initiate a collided unwind to halt unwinding.
_LIBUNWIND_EXPORT EXCEPTION_DISPOSITION
_GCC_specific_handler(PEXCEPTION_RECORD ms_exc, PVOID frame, PCONTEXT ms_ctx,
                      DISPATCHER_CONTEXT *disp, _Unwind_Personality_Fn pers) {
  unw_cursor_t cursor;
  _Unwind_Exception *exc;
  _Unwind_Action action;
  struct _Unwind_Context *ctx = nullptr;
  _Unwind_Reason_Code urc;
  uintptr_t retval, target;
  bool ours = false;
#if defined(_WIN32_ITANIUM)
  _Unwind_Exception foreign;
#endif

  _LIBUNWIND_TRACE_UNWINDING("_GCC_specific_handler(%#010lx(%lx), %p)",
                             ms_exc->ExceptionCode, ms_exc->ExceptionFlags,
                             (void *)frame);
  if (ms_exc->ExceptionCode == STATUS_GCC_UNWIND) {
    if (IS_TARGET_UNWIND(ms_exc->ExceptionFlags)) {
      // Set up the upper return value (the lower one and the target PC
      // were set in the call to RtlUnwindEx()) for the landing pad.
#ifdef __x86_64__
      disp->ContextRecord->Rdx = ms_exc->ExceptionInformation[3];
#elif defined(__arm__)
      disp->ContextRecord->R1 = ms_exc->ExceptionInformation[3];
#elif defined(__aarch64__)
      disp->ContextRecord->X1 = ms_exc->ExceptionInformation[3];
#endif
    }
    // This is the collided unwind to the landing pad. Nothing to do.
    return ExceptionContinueSearch;
  }

  if (ms_exc->ExceptionCode == STATUS_GCC_THROW) {
    // This is (probably) a libunwind-controlled exception/unwind. Recover the
    // parameters which we set below, and pass them to the personality function.
    ours = true;
    exc = (_Unwind_Exception *)ms_exc->ExceptionInformation[0];
    // Only __libunwind_seh_personality() passes three parameters. Its record
    // carries the unwinding flags when it runs a forced unwind.
    if (ms_exc->NumberParameters == 3) {
      ctx = (struct _Unwind_Context *)ms_exc->ExceptionInformation[1];
      action = (_Unwind_Action)ms_exc->ExceptionInformation[2];
    }
  } else {
    // Foreign exception.
    // We can't interact with them (we don't know the original target frame
    // that we should pass on to RtlUnwindEx in _Unwind_Resume), so just
    // pass without landing here. Cleanups that are funclets need no landing,
    // and run in every frame an unwind passes.
#if defined(_WIN32_ITANIUM)
    // A search asks the personality with an object that stands for the
    // record. The frame that takes the exception is unwound to with the
    // record itself, so that the frames between see the unwind of what was
    // raised, and with the object its landing pad is to receive as the return
    // value, by which the frame knows it when the unwind arrives. An unwind
    // that began for no exception arrives for no landing pad.
    bool exception = ms_exc->ExceptionCode != (DWORD)STATUS_UNWIND &&
                     ms_exc->ExceptionCode != (DWORD)STATUS_LONGJUMP &&
                     ms_exc->ExceptionCode != (DWORD)STATUS_UNWIND_CONSOLIDATE;
    if (exception && !IS_UNWINDING(ms_exc->ExceptionFlags)) {
      memset(&foreign, 0, sizeof(foreign));
      foreign.exception_class = _UNWIND_SEH_EXCEPTION_CLASS;
      foreign.private_[0] = (uintptr_t)ms_exc;
      foreign.private_[1] = (uintptr_t)ms_ctx;
      exc = &foreign;
    } else if (exception && IS_TARGET_UNWIND(ms_exc->ExceptionFlags)) {
#if defined(__x86_64__)
      exc = (_Unwind_Exception *)disp->ContextRecord->Rax;
#elif defined(__aarch64__)
      exc = (_Unwind_Exception *)disp->ContextRecord->X0;
#endif
    } else {
      if (!IS_TARGET_UNWIND(ms_exc->ExceptionFlags))
        callCleanups(ms_exc, frame, disp, disp->ControlPc);
      return ExceptionContinueSearch;
    }
#else
    return ExceptionContinueSearch;
#endif
  }
  if (!ctx) {
    __unw_init_seh(&cursor, disp->ContextRecord);
    __unw_seh_set_disp_ctx(&cursor, disp);
    __unw_set_reg(&cursor, UNW_REG_IP, disp->ControlPc);
    ctx = (struct _Unwind_Context *)&cursor;

    if (!IS_UNWINDING(ms_exc->ExceptionFlags)) {
      action = _UA_SEARCH_PHASE;
    } else {
      if (ours && ms_exc->ExceptionInformation[1] == (ULONG_PTR)frame)
        action = (_Unwind_Action)(_UA_CLEANUP_PHASE | _UA_HANDLER_FRAME);
      else
        action = _UA_CLEANUP_PHASE;
    }
  }
#if defined(_WIN32_ITANIUM)
  // A foreign exception's unwind gets here at its target alone.
  if (!ours && IS_UNWINDING(ms_exc->ExceptionFlags))
    action = (_Unwind_Action)(_UA_CLEANUP_PHASE | _UA_HANDLER_FRAME);
#endif

  _LIBUNWIND_TRACE_UNWINDING("_GCC_specific_handler() calling personality "
                             "function %p(1, %d, %llx, %p, %p)",
                             (void *)pers, action, exc->exception_class,
                             (void *)exc, (void *)ctx);
#if defined(_WIN32_ITANIUM)
  // The frame's address, which the cursor loses to the pad a personality
  // routine names.
  const uintptr_t pc = disp->ControlPc;
#endif
  urc = pers(1, action, exc->exception_class, exc, ctx);
  _LIBUNWIND_TRACE_UNWINDING("_GCC_specific_handler() personality returned %d", urc);
  switch (urc) {
  case _URC_CONTINUE_UNWIND:
    // If we're in phase 2, and the personality routine said to continue
    // at the target frame, we're in real trouble.
    if (action & _UA_HANDLER_FRAME)
      _LIBUNWIND_ABORT("Personality continued unwind at the target frame!");
#if defined(_WIN32_ITANIUM)
    if (action & _UA_CLEANUP_PHASE)
      callCleanups(ms_exc, frame, disp, pc);
#endif
    return ExceptionContinueSearch;
  case _URC_HANDLER_FOUND:
    // If we were called by __libunwind_seh_personality(), indicate that
    // a handler was found; otherwise, initiate phase 2 by unwinding.
    if (ours && ms_exc->NumberParameters == 3)
      return static_cast<EXCEPTION_DISPOSITION>(4);
    // This should never happen in phase 2.
    if (IS_UNWINDING(ms_exc->ExceptionFlags))
      _LIBUNWIND_ABORT("Personality indicated exception handler in phase 2!");
#if defined(_WIN32_ITANIUM)
    // The personality named the object the frame's landing pad receives.
    if (!ours)
      exc = (_Unwind_Exception *)foreign.private_[2];
#endif
    exc->private_[1] = (ULONG_PTR)frame;
    if (ours) {
      ms_exc->NumberParameters = 4;
      ms_exc->ExceptionInformation[1] = (ULONG_PTR)frame;
    }
    // FIXME: Indicate target frame in foreign case!
    // phase 2: the clean up phase
    RtlUnwindEx(frame, (PVOID)disp->ControlPc, ms_exc, exc, disp->ContextRecord,
                disp->HistoryTable);
    _LIBUNWIND_ABORT("RtlUnwindEx() failed");
  case _URC_INSTALL_CONTEXT: {
#if defined(_WIN32_ITANIUM)
    // The personality routine chose a handler by its selector. The frame lands
    // at that handler's pad once the cleanups inside its try block have run.
    if (action & _UA_CLEANUP_PHASE) {
      unw_word_t selector;
#if defined(__x86_64__)
      __unw_get_reg((unw_cursor_t *)ctx, UNW_X86_64_RDX, &selector);
#elif defined(__aarch64__)
      __unw_get_reg((unw_cursor_t *)ctx, UNW_AARCH64_X1, &selector);
#endif
      const int64_t handled = (int64_t)selector;
      uintptr_t pad = callCleanups(ms_exc, frame, disp, pc, &handled);
      if (!pad)
        _LIBUNWIND_ABORT("no handler of the frame takes what it was chosen for");
      __unw_set_reg((unw_cursor_t *)ctx, UNW_REG_IP, pad);
    }
#endif
    // If we were called by __libunwind_seh_personality(), indicate that
    // a handler was found; otherwise, it's time to initiate a collided
    // unwind to the target.
    if (ours && ms_exc->NumberParameters == 3)
      return static_cast<EXCEPTION_DISPOSITION>(4);
    // This should never happen in phase 1.
    if (!IS_UNWINDING(ms_exc->ExceptionFlags))
      _LIBUNWIND_ABORT("Personality installed context during phase 1!");
#ifdef __x86_64__
    exc->private_[2] = disp->TargetIp;
    __unw_get_reg(&cursor, UNW_X86_64_RAX, &retval);
    __unw_get_reg(&cursor, UNW_X86_64_RDX, &exc->private_[3]);
#elif defined(__arm__)
    exc->private_[2] = disp->TargetPc;
    __unw_get_reg(&cursor, UNW_ARM_R0, &retval);
    __unw_get_reg(&cursor, UNW_ARM_R1, &exc->private_[3]);
#elif defined(__aarch64__)
    exc->private_[2] = disp->TargetPc;
    __unw_get_reg(&cursor, UNW_AARCH64_X0, &retval);
    __unw_get_reg(&cursor, UNW_AARCH64_X1, &exc->private_[3]);
#endif
    __unw_get_reg(&cursor, UNW_REG_IP, &target);
    ms_exc->ExceptionCode = STATUS_GCC_UNWIND;
#ifdef __x86_64__
    ms_exc->ExceptionInformation[2] = disp->TargetIp;
#elif defined(__arm__) || defined(__aarch64__)
    ms_exc->ExceptionInformation[2] = disp->TargetPc;
#endif
    ms_exc->ExceptionInformation[3] = exc->private_[3];
    // Give NTRTL some scratch space to keep track of the collided unwind.
    // Don't use the one that was passed in; we don't want to overwrite the
    // context in the DISPATCHER_CONTEXT.
    CONTEXT new_ctx;
    RtlUnwindEx(frame, (PVOID)target, ms_exc, (PVOID)retval, &new_ctx, disp->HistoryTable);
    _LIBUNWIND_ABORT("RtlUnwindEx() failed");
  }
  // Anything else indicates a serious problem.
  default: return ExceptionContinueExecution;
  }
}

/// Personality function returned by \c __unw_get_proc_info() in SEH contexts.
/// This is a wrapper that calls the real SEH handler function, which in
/// turn (at least, for Itanium-style frames) calls the real Itanium
/// personality function (see \c _GCC_specific_handler()).
///
/// A language handler is called as the system's dispatcher calls it,
/// unchecked: Control Flow Guard does not list it as a target of indirect
/// calls, and a foreign one carries no KCFI type.
extern "C" __attribute__((guard(nocf), no_sanitize("kcfi"))) _Unwind_Reason_Code
__libunwind_seh_personality(int version, _Unwind_Action state,
                            uint64_t klass, _Unwind_Exception *exc,
                            struct _Unwind_Context *context) {
  (void)version;
  (void)klass;
  EXCEPTION_RECORD ms_exc;
  bool phase2 = (state & (_UA_SEARCH_PHASE|_UA_CLEANUP_PHASE)) == _UA_CLEANUP_PHASE;
  ms_exc.ExceptionCode = STATUS_GCC_THROW;
  // A forced unwind has no handler to search for: a foreign frame is told it
  // is being unwound towards the end of the stack, so its termination
  // handlers run and its exception filters are not consulted.
  ms_exc.ExceptionFlags = (state & _UA_FORCE_UNWIND)
                              ? EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND
                              : 0;
  ms_exc.NumberParameters = 3;
  ms_exc.ExceptionInformation[0] = (ULONG_PTR)exc;
  ms_exc.ExceptionInformation[1] = (ULONG_PTR)context;
  ms_exc.ExceptionInformation[2] = state;
  DISPATCHER_CONTEXT *disp_ctx =
      __unw_seh_get_disp_ctx((unw_cursor_t *)context);
#if defined(__aarch64__)
  LOCAL_DISPATCHER_CONTEXT_NONVOLREG_ARM64 nonvol;
  memcpy(&nonvol.GpNvRegs, &disp_ctx->ContextRecord->X19,
         sizeof(nonvol.GpNvRegs));
  for (int i = 0; i < 8; i++)
    nonvol.FpNvRegs[i] = disp_ctx->ContextRecord->V[i + 8].D[0];
  disp_ctx->NonVolatileRegisters = nonvol.Buffer;
#elif defined(__arm__)
  LOCAL_DISPATCHER_CONTEXT_NONVOLREG_ARM nonvol;
  memcpy(&nonvol.GpNvRegs, &disp_ctx->ContextRecord->R4,
         sizeof(nonvol.GpNvRegs));
  memcpy(&nonvol.FpNvRegs, &disp_ctx->ContextRecord->D[8],
         sizeof(nonvol.FpNvRegs));
  disp_ctx->NonVolatileRegisters = nonvol.Buffer;
#endif
  _LIBUNWIND_TRACE_UNWINDING("__libunwind_seh_personality() calling "
                             "LanguageHandler %p(%p, %p, %p, %p)",
                             (void *)disp_ctx->LanguageHandler, (void *)&ms_exc,
                             (void *)disp_ctx->EstablisherFrame,
                             (void *)disp_ctx->ContextRecord, (void *)disp_ctx);
  // The cursor holds the establisher frame of the frame it last left. A
  // handler takes its own frame's.
  CONTEXT caller = *disp_ctx->ContextRecord;
  PVOID data;
  RtlVirtualUnwind(UNW_FLAG_NHANDLER, disp_ctx->ImageBase, disp_ctx->ControlPc,
                   disp_ctx->FunctionEntry, &caller, &data,
                   &disp_ctx->EstablisherFrame, nullptr);
  CONTEXT *ms_ctx = disp_ctx->ContextRecord;
  UNWIND_HISTORY_TABLE *history = disp_ctx->HistoryTable;
  int ms_act;
  // An unwind that leaves a handler another unwind called has collided with
  // it. The system's handler for that call names the frame whose handler it
  // was: the walk goes on from that frame, whose handler is told so.
  while ((ms_act = static_cast<int>(disp_ctx->LanguageHandler(
              &ms_exc, (PVOID)disp_ctx->EstablisherFrame,
              disp_ctx->ContextRecord, disp_ctx))) == ExceptionCollidedUnwind) {
    *ms_ctx = *disp_ctx->ContextRecord;
    disp_ctx->ContextRecord = ms_ctx;
    disp_ctx->HistoryTable = history;
    ms_exc.ExceptionFlags |= EXCEPTION_COLLIDED_UNWIND;
  }
  _LIBUNWIND_TRACE_UNWINDING("__libunwind_seh_personality() LanguageHandler "
                             "returned %d",
                             ms_act);
  switch (ms_act) {
  case ExceptionContinueExecution: return _URC_END_OF_STACK;
  case ExceptionContinueSearch: return _URC_CONTINUE_UNWIND;
  case 4 /*ExceptionExecuteHandler*/:
    return phase2 ? _URC_INSTALL_CONTEXT : _URC_HANDLER_FOUND;
  default:
    return phase2 ? _URC_FATAL_PHASE2_ERROR : _URC_FATAL_PHASE1_ERROR;
  }
}

static _Unwind_Reason_Code
unwind_phase2_forced(unw_context_t *uc,
                     _Unwind_Exception *exception_object,
                     _Unwind_Stop_Fn stop, void *stop_parameter) {
  unw_cursor_t cursor2;
  __unw_init_local(&cursor2, uc);

  // Walk each frame until we reach where search phase said to stop
  while (__unw_step(&cursor2) > 0) {

    // Update info about this frame.
    unw_proc_info_t frameInfo;
    if (__unw_get_proc_info(&cursor2, &frameInfo) != UNW_ESUCCESS) {
      _LIBUNWIND_TRACE_UNWINDING("unwind_phase2_forced(ex_ojb=%p): __unw_get_proc_info "
                                 "failed => _URC_END_OF_STACK",
                                 (void *)exception_object);
      return _URC_FATAL_PHASE2_ERROR;
    }

#ifndef NDEBUG
    // When tracing, print state information.
    if (_LIBUNWIND_TRACING_UNWINDING) {
      char functionBuf[512];
      const char *functionName = functionBuf;
      unw_word_t offset;
      if ((__unw_get_proc_name(&cursor2, functionBuf, sizeof(functionBuf),
                               &offset) != UNW_ESUCCESS) ||
          (frameInfo.start_ip + offset > frameInfo.end_ip))
        functionName = ".anonymous.";
      _LIBUNWIND_TRACE_UNWINDING(
          "unwind_phase2_forced(ex_ojb=%p): start_ip=0x%" PRIxPTR
          ", func=%s, lsda=0x%" PRIxPTR ", personality=0x%" PRIxPTR,
          (void *)exception_object, frameInfo.start_ip, functionName,
          frameInfo.lsda, frameInfo.handler);
    }
#endif

    // Call stop function at each frame.
    _Unwind_Action action =
        (_Unwind_Action)(_UA_FORCE_UNWIND | _UA_CLEANUP_PHASE);
    _Unwind_Reason_Code stopResult =
        (*stop)(1, action, exception_object->exception_class, exception_object,
                (struct _Unwind_Context *)(&cursor2), stop_parameter);
    _LIBUNWIND_TRACE_UNWINDING(
        "unwind_phase2_forced(ex_ojb=%p): stop function returned %d",
        (void *)exception_object, stopResult);
    if (stopResult != _URC_NO_REASON) {
      _LIBUNWIND_TRACE_UNWINDING(
          "unwind_phase2_forced(ex_ojb=%p): stopped by stop function",
          (void *)exception_object);
      return _URC_FATAL_PHASE2_ERROR;
    }

    // If there is a personality routine, tell it we are unwinding.
    if (frameInfo.handler != 0) {
      _Unwind_Personality_Fn p =
          (_Unwind_Personality_Fn)(intptr_t)(frameInfo.handler);
      _LIBUNWIND_TRACE_UNWINDING(
          "unwind_phase2_forced(ex_ojb=%p): calling personality function %p",
          (void *)exception_object, (void *)(uintptr_t)p);
      _Unwind_Reason_Code personalityResult =
          (*p)(1, action, exception_object->exception_class, exception_object,
               (struct _Unwind_Context *)(&cursor2));
      switch (personalityResult) {
      case _URC_CONTINUE_UNWIND:
        _LIBUNWIND_TRACE_UNWINDING("unwind_phase2_forced(ex_ojb=%p): "
                                   "personality returned "
                                   "_URC_CONTINUE_UNWIND",
                                   (void *)exception_object);
        // Destructors called, continue unwinding
        break;
      case _URC_INSTALL_CONTEXT:
        _LIBUNWIND_TRACE_UNWINDING("unwind_phase2_forced(ex_ojb=%p): "
                                   "personality returned "
                                   "_URC_INSTALL_CONTEXT",
                                   (void *)exception_object);
        // We may get control back if landing pad calls _Unwind_Resume().
        __unw_resume(&cursor2);
        break;
      case _URC_END_OF_STACK:
        _LIBUNWIND_TRACE_UNWINDING("unwind_phase2_forced(ex_ojb=%p): "
                                   "personality returned "
                                   "_URC_END_OF_STACK",
                                   (void *)exception_object);
        break;
      default:
        // Personality routine returned an unknown result code.
        _LIBUNWIND_TRACE_UNWINDING("unwind_phase2_forced(ex_ojb=%p): "
                                   "personality returned %d, "
                                   "_URC_FATAL_PHASE2_ERROR",
                                   (void *)exception_object, personalityResult);
        return _URC_FATAL_PHASE2_ERROR;
      }
      if (personalityResult == _URC_END_OF_STACK)
        break;
    }
  }

  // Call stop function one last time and tell it we've reached the end
  // of the stack.
  _LIBUNWIND_TRACE_UNWINDING("unwind_phase2_forced(ex_ojb=%p): calling stop "
                             "function with _UA_END_OF_STACK",
                             (void *)exception_object);
  _Unwind_Action lastAction =
      (_Unwind_Action)(_UA_FORCE_UNWIND | _UA_CLEANUP_PHASE | _UA_END_OF_STACK);
  (*stop)(1, lastAction, exception_object->exception_class, exception_object,
          (struct _Unwind_Context *)(&cursor2), stop_parameter);

  // Clean up phase did not resume at the frame that the search phase said it
  // would.
  return _URC_FATAL_PHASE2_ERROR;
}

/// Called by \c __cxa_throw().  Only returns if there is a fatal error.
_LIBUNWIND_EXPORT _Unwind_Reason_Code
_Unwind_RaiseException(_Unwind_Exception *exception_object) {
  _LIBUNWIND_TRACE_API("_Unwind_RaiseException(ex_obj=%p)",
                       (void *)exception_object);

  // Mark that this is a non-forced unwind, so _Unwind_Resume()
  // can do the right thing.
  memset(exception_object->private_, 0, sizeof(exception_object->private_));

  // phase 1: the search phase
  // We'll let the system do that for us. RtlRaiseException(), unlike
  // RaiseException(), records this function as the exception's address, by
  // which an unhandled-exception filter knows a raise that it can resume.
  EXCEPTION_RECORD ms_exc;
  memset(&ms_exc, 0, sizeof(ms_exc));
  ms_exc.ExceptionCode = STATUS_GCC_THROW;
  ms_exc.NumberParameters = 1;
  ms_exc.ExceptionInformation[0] = (ULONG_PTR)exception_object;
  RtlRaiseException(&ms_exc);

  // If we get here, either something went horribly wrong or we reached the
  // top of the stack. Either way, let libc++abi call std::terminate().
  return _URC_END_OF_STACK;
}

#if defined(_WIN32_ITANIUM)
/// The functions that raise an exception's search, for the start-up library's
/// unhandled-exception filter, which resumes a raise by either so that the
/// thrower calls std::terminate(). It finds them here when libunwind is linked
/// into the executable, which exports neither.
extern "C" _LIBUNWIND_HIDDEN const void *const __unw_seh_raise_functions[2] = {
    reinterpret_cast<const void *>(&_Unwind_RaiseException),
    reinterpret_cast<const void *>(&_Unwind_Resume_or_Rethrow)};
#endif

/// When \c _Unwind_RaiseException() is in phase2, it hands control
/// to the personality function at each frame.  The personality
/// may force a jump to a landing pad in that function; the landing
/// pad code may then call \c _Unwind_Resume() to continue with the
/// unwinding.  Note: the call to \c _Unwind_Resume() is from compiler
/// generated user code.  All other \c _Unwind_* routines are called
/// by the C++ runtime \c __cxa_* routines.
///
/// Note: re-throwing an exception (as opposed to continuing the unwind)
/// is implemented by having the code call \c __cxa_rethrow() which
/// in turn calls \c _Unwind_Resume_or_Rethrow().
_LIBUNWIND_EXPORT void
_Unwind_Resume(_Unwind_Exception *exception_object) {
  _LIBUNWIND_TRACE_API("_Unwind_Resume(ex_obj=%p)", (void *)exception_object);

  if (exception_object->private_[0] != 0) {
    unw_context_t uc;

    __unw_getcontext(&uc);
    unwind_phase2_forced(&uc, exception_object,
                         (_Unwind_Stop_Fn) exception_object->private_[0],
                         (void *)exception_object->private_[4]);
  } else {
    // Recover the parameters for the unwind from the exception object
    // so we can start unwinding again.
    EXCEPTION_RECORD ms_exc;
    CONTEXT ms_ctx;
    UNWIND_HISTORY_TABLE hist;

    memset(&ms_exc, 0, sizeof(ms_exc));
    memset(&hist, 0, sizeof(hist));
    ms_exc.ExceptionCode = STATUS_GCC_THROW;
    ms_exc.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    ms_exc.NumberParameters = 4;
    ms_exc.ExceptionInformation[0] = (ULONG_PTR)exception_object;
    ms_exc.ExceptionInformation[1] = exception_object->private_[1];
    ms_exc.ExceptionInformation[2] = exception_object->private_[2];
    ms_exc.ExceptionInformation[3] = exception_object->private_[3];
    RtlUnwindEx((PVOID)exception_object->private_[1],
                (PVOID)exception_object->private_[2], &ms_exc,
                exception_object, &ms_ctx, &hist);
  }

  // Clients assume _Unwind_Resume() does not return, so all we can do is abort.
  _LIBUNWIND_ABORT("_Unwind_Resume() can't return");
}

/// Not used by C++.
/// Unwinds stack, calling "stop" function at each frame.
/// Could be used to implement \c longjmp().
_LIBUNWIND_EXPORT _Unwind_Reason_Code
_Unwind_ForcedUnwind(_Unwind_Exception *exception_object,
                     _Unwind_Stop_Fn stop, void *stop_parameter) {
  _LIBUNWIND_TRACE_API("_Unwind_ForcedUnwind(ex_obj=%p, stop=%p)",
                       (void *)exception_object, (void *)(uintptr_t)stop);
  unw_context_t uc;
  __unw_getcontext(&uc);

  // Mark that this is a forced unwind, so _Unwind_Resume() can do
  // the right thing.
  exception_object->private_[0] = (uintptr_t) stop;
  exception_object->private_[4] = (uintptr_t) stop_parameter;

  // do it
  return unwind_phase2_forced(&uc, exception_object, stop, stop_parameter);
}

/// Called by personality handler during phase 2 to get LSDA for current frame.
_LIBUNWIND_EXPORT uintptr_t
_Unwind_GetLanguageSpecificData(struct _Unwind_Context *context) {
  uintptr_t result = getLSDA(__unw_seh_get_disp_ctx((unw_cursor_t *)context));
  _LIBUNWIND_TRACE_API(
      "_Unwind_GetLanguageSpecificData(context=%p) => 0x%" PRIxPTR,
      (void *)context, result);
  return result;
}

/// Called by personality handler during phase 2 to find the start of the
/// function.
_LIBUNWIND_EXPORT uintptr_t
_Unwind_GetRegionStart(struct _Unwind_Context *context) {
  DISPATCHER_CONTEXT *disp = __unw_seh_get_disp_ctx((unw_cursor_t *)context);
  uintptr_t result = (uintptr_t)disp->FunctionEntry->BeginAddress + disp->ImageBase;
  _LIBUNWIND_TRACE_API("_Unwind_GetRegionStart(context=%p) => 0x%" PRIxPTR,
                       (void *)context, result);
  return result;
}

static int __unw_init_seh(unw_cursor_t *cursor, CONTEXT *context) {
#ifdef _LIBUNWIND_TARGET_X86_64
  new (reinterpret_cast<UnwindCursor<LocalAddressSpace, Registers_x86_64> *>(cursor))
      UnwindCursor<LocalAddressSpace, Registers_x86_64>(
          context, LocalAddressSpace::sThisAddressSpace);
  auto *co = reinterpret_cast<AbstractUnwindCursor *>(cursor);
  co->setInfoBasedOnIPRegister();
  return UNW_ESUCCESS;
#elif defined(_LIBUNWIND_TARGET_ARM)
  new (reinterpret_cast<UnwindCursor<LocalAddressSpace, Registers_arm> *>(cursor))
      UnwindCursor<LocalAddressSpace, Registers_arm>(
          context, LocalAddressSpace::sThisAddressSpace);
  auto *co = reinterpret_cast<AbstractUnwindCursor *>(cursor);
  co->setInfoBasedOnIPRegister();
  return UNW_ESUCCESS;
#elif defined(_LIBUNWIND_TARGET_AARCH64)
  new (reinterpret_cast<UnwindCursor<LocalAddressSpace, Registers_arm64> *>(cursor))
      UnwindCursor<LocalAddressSpace, Registers_arm64>(
          context, LocalAddressSpace::sThisAddressSpace);
  auto *co = reinterpret_cast<AbstractUnwindCursor *>(cursor);
  co->setInfoBasedOnIPRegister();
  return UNW_ESUCCESS;
#else
  return UNW_EINVAL;
#endif
}

static DISPATCHER_CONTEXT *__unw_seh_get_disp_ctx(unw_cursor_t *cursor) {
#ifdef _LIBUNWIND_TARGET_X86_64
  return reinterpret_cast<UnwindCursor<LocalAddressSpace, Registers_x86_64> *>(cursor)->getDispatcherContext();
#elif defined(_LIBUNWIND_TARGET_ARM)
  return reinterpret_cast<UnwindCursor<LocalAddressSpace, Registers_arm> *>(cursor)->getDispatcherContext();
#elif defined(_LIBUNWIND_TARGET_AARCH64)
  return reinterpret_cast<UnwindCursor<LocalAddressSpace, Registers_arm64> *>(cursor)->getDispatcherContext();
#else
  return nullptr;
#endif
}

static void __unw_seh_set_disp_ctx(unw_cursor_t *cursor,
                                   DISPATCHER_CONTEXT *disp) {
#ifdef _LIBUNWIND_TARGET_X86_64
  reinterpret_cast<UnwindCursor<LocalAddressSpace, Registers_x86_64> *>(cursor)->setDispatcherContext(disp);
#elif defined(_LIBUNWIND_TARGET_ARM)
  reinterpret_cast<UnwindCursor<LocalAddressSpace, Registers_arm> *>(cursor)->setDispatcherContext(disp);
#elif defined(_LIBUNWIND_TARGET_AARCH64)
  reinterpret_cast<UnwindCursor<LocalAddressSpace, Registers_arm64> *>(cursor)->setDispatcherContext(disp);
#endif
}

#endif // defined(_LIBUNWIND_SUPPORT_SEH_UNWIND)
