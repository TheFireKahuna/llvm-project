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

#if defined(_WIN32_ITANIUM)
/// Exception class of the object libunwind hands to a C++ frame for an SEH
/// exception nobody translated. It matches no C++ runtime's class, so the
/// personality treats it as foreign: only catch (...) accepts it.
/// ASCII "LLVMSEH\0".
#define SEH_EXCEPTION_CLASS 0x4C4C564D53454800ULL

/// Value of private_[5] on every exception object that stands for an SEH
/// exception, translated or not. While such an object unwinds, frames whose
/// call-site table has no entry for the return address are passed by instead
/// of being handed to the personality, which would terminate.
#define SEH_ORIGIN_MARK ((uintptr_t)0x53454831) // "SEH1"

// Fixed NT status values, for header sets that leave them out.
#ifndef STATUS_UNWIND
#define STATUS_UNWIND ((NTSTATUS)0xC0000027L)
#endif
#ifndef STATUS_LONGJUMP
#define STATUS_LONGJUMP ((NTSTATUS)0x80000026L)
#endif
#ifndef STATUS_UNWIND_CONSOLIDATE
#define STATUS_UNWIND_CONSOLIDATE ((NTSTATUS)0x80000029L)
#endif
#ifndef STATUS_BREAKPOINT
#define STATUS_BREAKPOINT ((NTSTATUS)0x80000003L)
#endif

#if defined(__x86_64__)
#define CONTEXT_PC(ctx) ((ULONG_PTR)(ctx)->Rip)
#define CONTEXT_RETVAL(ctx) ((ULONG_PTR)(ctx)->Rax)
#elif defined(__aarch64__)
#define CONTEXT_PC(ctx) ((ULONG_PTR)(ctx)->Pc)
#define CONTEXT_RETVAL(ctx) ((ULONG_PTR)(ctx)->X0)
#elif defined(__arm__)
#define CONTEXT_PC(ctx) ((ULONG_PTR)(ctx)->Pc)
#define CONTEXT_RETVAL(ctx) ((ULONG_PTR)(ctx)->R0)
#endif

namespace {
/// What libunwind knows about an SEH exception it is carrying through C++
/// frames. For a catch (...) the object is on the heap and the personality
/// deletes it through exception_cleanup. For a cleanup during an unwind that
/// an __except or __finally frame drives, the object is a per-thread slot and
/// the target fields say where that unwind was going.
struct SEHExceptionState {
  _Unwind_Exception unwind;
  EXCEPTION_RECORD record;
  ULONG_PTR targetFrame;
  ULONG_PTR targetIp;
  ULONG_PTR returnValue;
  ULONG_PTR lastFrame;
};

void sehHeapCleanup(_Unwind_Reason_Code, _Unwind_Exception *exc) { free(exc); }
void sehSlotCleanup(_Unwind_Reason_Code, _Unwind_Exception *) {}

bool isSEHException(const _Unwind_Exception *exc) {
  return exc->exception_class == SEH_EXCEPTION_CLASS;
}

bool isSEHOrigin(const _Unwind_Exception *exc) {
  return exc->private_[5] == SEH_ORIGIN_MARK;
}

/// Slots for the unwinds an __except or __finally frame drives through C++
/// cleanups. Depth counts the unwinds on this thread that may still resume;
/// one nests inside another only when a destructor run by the first raises
/// an SEH exception that an __except inside that destructor takes.
struct SEHUnwindStack {
  SEHExceptionState slot[4];
  unsigned depth;
};
thread_local SEHUnwindStack sehUnwinds;

/// The translator a language runtime registers: builds an exception object
/// for an SEH exception, or returns null to leave it foreign; and the
/// function that gives the record to raise again when such an object is
/// rethrown.
_Unwind_SEH_Translator sehTranslator = nullptr;
_Unwind_SEH_Rethrow sehRethrow = nullptr;

/// Whether the frame's call-site table has an entry containing an address:
/// the byte before the return address, as the personality looks it up, or
/// the faulting instruction itself in the frame where a hardware exception
/// occurred. A C++ personality terminates the process when there is none,
/// because a C++ exception can only get there through a call the compiler
/// marked as not throwing. An SEH exception can get there through any call
/// or instruction, so frames without an entry are passed by without
/// consulting the personality.
bool callSiteTableCovers(DISPATCHER_CONTEXT *disp, ULONG_PTR address) {
  typedef LocalAddressSpace::pint_t pint_t;
  LocalAddressSpace &as = LocalAddressSpace::sThisAddressSpace;
  pint_t p = (pint_t)disp->HandlerData;
  if (p == 0)
    return false;
  const pint_t noEnd = (pint_t)-1;
  uint8_t lpStartEncoding = as.get8(p++);
  if (lpStartEncoding != DW_EH_PE_omit)
    as.getEncodedP(p, noEnd, lpStartEncoding, 0);
  uint8_t ttypeEncoding = as.get8(p++);
  if (ttypeEncoding != DW_EH_PE_omit)
    as.getULEB128(p, noEnd);
  uint8_t callSiteEncoding = as.get8(p++);
  pint_t callSiteLength = (pint_t)as.getULEB128(p, noEnd);
  pint_t callSiteEnd = p + callSiteLength;
  pint_t funcStart = disp->ImageBase + disp->FunctionEntry->BeginAddress;
  pint_t ipOffset = address - funcStart;
  while (p < callSiteEnd) {
    pint_t start = as.getEncodedP(p, callSiteEnd, callSiteEncoding, 0);
    pint_t length = as.getEncodedP(p, callSiteEnd, callSiteEncoding, 0);
    as.getEncodedP(p, callSiteEnd, callSiteEncoding, 0);
    as.getULEB128(p, callSiteEnd);
    if (ipOffset < start)
      return false;
    if (ipOffset < start + length)
      return true;
  }
  return false;
}

/// Start of the function an entry belongs to. On x86-64 an entry may chain
/// to the one for an earlier part of the same function.
ULONG_PTR functionStart(const RUNTIME_FUNCTION *fe, ULONG_PTR base) {
#if defined(__x86_64__)
  for (unsigned depth = 0; depth < 8; ++depth) {
    const uint8_t *info = (const uint8_t *)(base + fe->UnwindData);
    if (((info[0] >> 3) & UNW_FLAG_CHAININFO) == 0)
      break;
    fe = (const RUNTIME_FUNCTION *)(info + 4 + ((info[2] + 1u) & ~1u) * 2);
  }
#endif
  return base + fe->BeginAddress;
}

/// The bounds of the current thread's stack, from the TEB.
void currentStackBounds(ULONG_PTR &limit, ULONG_PTR &base) {
  NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
  base = (ULONG_PTR)tib->StackBase;
  limit = (ULONG_PTR)tib->StackLimit;
}

/// The frame an unwind in progress is heading for. The dispatcher context
/// carries the target's resume address but not its frame, so the frame is
/// found by walking the callers of the current frame for the activation of
/// the function that contains that address. If that function has more than
/// one activation on the stack, the filters that chose the target cannot be
/// re-asked, and the walk gives up.
ULONG_PTR findTargetFrame(DISPATCHER_CONTEXT *disp) {
  DWORD64 base;
  RUNTIME_FUNCTION *fe =
      RtlLookupFunctionEntry(disp->TargetIp, &base, disp->HistoryTable);
  if (!fe)
    return 0;
  ULONG_PTR targetFunction = functionStart(fe, (ULONG_PTR)base);
  ULONG_PTR stackLimit, stackBase;
  currentStackBounds(stackLimit, stackBase);
  CONTEXT ctx = *disp->ContextRecord;
  ULONG_PTR found = 0;
  ULONG_PTR previousFrame = disp->EstablisherFrame;
  bool first = true;
  for (unsigned steps = 0; steps < 4096; ++steps) {
    ULONG_PTR pc = CONTEXT_PC(&ctx);
    if (pc == 0)
      break;
    fe = RtlLookupFunctionEntry(first ? pc : pc - 1, &base, disp->HistoryTable);
    if (!fe)
      break;
    PVOID handlerData;
    ULONG_PTR establisher;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, pc, fe, &ctx, &handlerData,
                     &establisher, nullptr);
    if (establisher < stackLimit || establisher >= stackBase ||
        (!first && establisher <= previousFrame))
      break;
    if (!first && functionStart(fe, (ULONG_PTR)base) == targetFunction) {
      if (found)
        return 0;
      found = establisher;
    }
    previousFrame = establisher;
    first = false;
  }
  return found;
}
} // namespace
#endif // defined(_WIN32_ITANIUM)

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

#if defined(_WIN32_ITANIUM)
/// Points the cursor's IP where the personality expects a return address:
/// in the frame where a hardware exception occurred, one past the faulting
/// instruction, so that the byte before it is the instruction itself.
static void setPersonalityIP(unw_cursor_t *cursor, DISPATCHER_CONTEXT *disp,
                             bool faultFrame) {
  __unw_set_reg(cursor, UNW_REG_IP, disp->ControlPc + (faultFrame ? 1 : 0));
}

/// Runs the cleanups of a frame that an unwind driven by an __except or
/// __finally frame is passing through. The landing pad cannot return into
/// that unwind, so the frame is entered through a collided unwind and
/// _Unwind_Resume() starts the original unwind again from where it stopped.
static EXCEPTION_DISPOSITION
unwindForeignFrame(PEXCEPTION_RECORD ms_exc, PVOID frame, bool faultFrame,
                   DISPATCHER_CONTEXT *disp, _Unwind_Personality_Fn pers) {
  SEHUnwindStack &stack = sehUnwinds;
  // An unwind reaching a frame at or above the one another unwind stopped
  // at means that other unwind has finished or was abandoned.
  while (stack.depth != 0 &&
         stack.slot[stack.depth - 1].lastFrame <= (ULONG_PTR)frame)
    --stack.depth;
  if (stack.depth == sizeof(stack.slot) / sizeof(stack.slot[0]))
    return ExceptionContinueSearch;
  ULONG_PTR targetFrame = findTargetFrame(disp);
  if (targetFrame == 0)
    return ExceptionContinueSearch;

  SEHExceptionState *state = &stack.slot[stack.depth];
  memset(&state->unwind, 0, sizeof(state->unwind));
  state->unwind.exception_class = SEH_EXCEPTION_CLASS;
  state->unwind.exception_cleanup = sehSlotCleanup;
  state->unwind.private_[5] = SEH_ORIGIN_MARK;
  state->record = *ms_exc;
  state->record.ExceptionRecord = nullptr;
  state->targetFrame = targetFrame;
  state->targetIp = disp->TargetIp;
  // The unwinder places its return value in the frame context before every
  // handler call; for __C_specific_handler that is the exception code.
  state->returnValue = CONTEXT_RETVAL(disp->ContextRecord);
  state->lastFrame = (ULONG_PTR)frame;
  ++stack.depth;

  unw_cursor_t cursor;
  __unw_init_seh(&cursor, disp->ContextRecord);
  __unw_seh_set_disp_ctx(&cursor, disp);
  setPersonalityIP(&cursor, disp, faultFrame);
  _LIBUNWIND_TRACE_UNWINDING("_GCC_specific_handler() foreign unwind at %p "
                             "towards %p", (void *)frame, (void *)targetFrame);
  _Unwind_Reason_Code urc =
      pers(1, _UA_CLEANUP_PHASE, SEH_EXCEPTION_CLASS, &state->unwind,
           (struct _Unwind_Context *)&cursor);
  if (urc != _URC_INSTALL_CONTEXT) {
    --stack.depth;
    return ExceptionContinueSearch;
  }
  uintptr_t retval, target;
#if defined(__x86_64__)
  __unw_get_reg(&cursor, UNW_X86_64_RAX, &retval);
  __unw_get_reg(&cursor, UNW_X86_64_RDX, &state->unwind.private_[3]);
#elif defined(__arm__)
  __unw_get_reg(&cursor, UNW_ARM_R0, &retval);
  __unw_get_reg(&cursor, UNW_ARM_R1, &state->unwind.private_[3]);
#elif defined(__aarch64__)
  __unw_get_reg(&cursor, UNW_AARCH64_X0, &retval);
  __unw_get_reg(&cursor, UNW_AARCH64_X1, &state->unwind.private_[3]);
#endif
  // A selector other than zero means the personality chose a catch, which
  // the system's search pass did not offer this frame. The unwind belongs
  // to the frame that took the exception; only cleanups run here.
  if (state->unwind.private_[3] != 0) {
    --stack.depth;
    return ExceptionContinueSearch;
  }
  __unw_get_reg(&cursor, UNW_REG_IP, &target);
  EXCEPTION_RECORD rec;
  memset(&rec, 0, sizeof(rec));
  rec.ExceptionCode = STATUS_GCC_UNWIND;
  rec.NumberParameters = 4;
  rec.ExceptionInformation[3] = state->unwind.private_[3];
  CONTEXT new_ctx;
  RtlUnwindEx(frame, (PVOID)target, &rec, (PVOID)retval, &new_ctx,
              disp->HistoryTable);
  _LIBUNWIND_ABORT("RtlUnwindEx() failed");
}

/// Asks a frame's personality, while the system is still looking for a
/// handler, whether the frame would catch the SEH exception. If it would,
/// takes the exception over: unwinds to the frame as for a C++ exception,
/// so the frames in between run their cleanups and the landing pad receives
/// either the object the translator built or a foreign one standing for the
/// SEH exception.
static EXCEPTION_DISPOSITION
searchForeignFrame(PEXCEPTION_RECORD ms_exc, PVOID frame, PCONTEXT ms_ctx,
                   bool faultFrame, DISPATCHER_CONTEXT *disp,
                   _Unwind_Personality_Fn pers) {
  unw_cursor_t cursor;
  __unw_init_seh(&cursor, disp->ContextRecord);
  __unw_seh_set_disp_ctx(&cursor, disp);
  setPersonalityIP(&cursor, disp, faultFrame);

  _Unwind_Exception *exc = nullptr;
  if (sehTranslator)
    exc = sehTranslator(ms_exc, ms_ctx);
  bool translated = exc != nullptr;
  _Unwind_Exception probe;
  if (!translated) {
    memset(&probe, 0, sizeof(probe));
    probe.exception_class = SEH_EXCEPTION_CLASS;
    exc = &probe;
  }
  _LIBUNWIND_TRACE_UNWINDING("_GCC_specific_handler() probing %p for a "
                             "handler of SEH exception %#010lx",
                             (void *)frame, ms_exc->ExceptionCode);
  _Unwind_Reason_Code urc =
      pers(1, _UA_SEARCH_PHASE, exc->exception_class, exc,
           (struct _Unwind_Context *)&cursor);
  if (urc != _URC_HANDLER_FOUND) {
    if (translated)
      _Unwind_DeleteException(exc);
    return ExceptionContinueSearch;
  }
  if (!translated) {
    SEHExceptionState *state =
        (SEHExceptionState *)malloc(sizeof(SEHExceptionState));
    if (!state)
      return ExceptionContinueSearch;
    memset(state, 0, sizeof(*state));
    state->unwind.exception_class = SEH_EXCEPTION_CLASS;
    state->unwind.exception_cleanup = sehHeapCleanup;
    state->record = *ms_exc;
    state->record.ExceptionRecord = nullptr;
    exc = &state->unwind;
  }
  memset(exc->private_, 0, sizeof(exc->private_));
  exc->private_[1] = (ULONG_PTR)frame;
  // The unwind that follows meets the faulting frame again and must offer
  // the personality the same view of it.
  exc->private_[4] = faultFrame ? disp->ControlPc : 0;
  exc->private_[5] = SEH_ORIGIN_MARK;
  EXCEPTION_RECORD rec;
  memset(&rec, 0, sizeof(rec));
  rec.ExceptionCode = STATUS_GCC_THROW;
  rec.NumberParameters = 4;
  rec.ExceptionInformation[0] = (ULONG_PTR)exc;
  rec.ExceptionInformation[1] = (ULONG_PTR)frame;
  RtlUnwindEx(frame, (PVOID)disp->ControlPc, &rec, exc, disp->ContextRecord,
              disp->HistoryTable);
  _LIBUNWIND_ABORT("RtlUnwindEx() failed");
}

_LIBUNWIND_EXPORT void
_Unwind_SetSEHTranslator(_Unwind_SEH_Translator translator,
                         _Unwind_SEH_Rethrow rethrow) {
  sehTranslator = translator;
  sehRethrow = rethrow;
}

/// An exception that is not libunwind's own.
static EXCEPTION_DISPOSITION
handleForeignException(PEXCEPTION_RECORD ms_exc, PVOID frame, PCONTEXT ms_ctx,
                       DISPATCHER_CONTEXT *disp, _Unwind_Personality_Fn pers) {
  switch (ms_exc->ExceptionCode) {
  // Unwinds that are not exceptions: longjmp, RtlUnwind, and the record an
  // MSVC C++ frame uses to enter a catch. Their resume point is not the
  // dispatcher context's target, so they pass through, as they do in MSVC
  // code built without /EHa.
  case (DWORD)STATUS_UNWIND:
  case (DWORD)STATUS_LONGJUMP:
  case (DWORD)STATUS_UNWIND_CONSOLIDATE:
  // A breakpoint is for the debugger, never for a handler.
  case (DWORD)STATUS_BREAKPOINT:
    return ExceptionContinueSearch;
  default:
    break;
  }
  // In the frame where a hardware exception occurred, ControlPc is the
  // faulting instruction rather than a return address. Only a table that
  // covers that instruction itself, as one built for asynchronous exceptions
  // does, has anything to say about it.
  bool faultFrame = (ULONG_PTR)ms_exc->ExceptionAddress == disp->ControlPc;
  if (!callSiteTableCovers(disp, disp->ControlPc - (faultFrame ? 0 : 1)))
    return ExceptionContinueSearch;
  if (IS_UNWINDING(ms_exc->ExceptionFlags)) {
    // An exit unwind has no target to resume towards.
    if (ms_exc->ExceptionFlags & EXCEPTION_EXIT_UNWIND)
      return ExceptionContinueSearch;
    return unwindForeignFrame(ms_exc, frame, faultFrame, disp, pers);
  }
  return searchForeignFrame(ms_exc, frame, ms_ctx, faultFrame, disp, pers);
}
#endif // defined(_WIN32_ITANIUM)

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
#if defined(_WIN32_ITANIUM)
    return handleForeignException(ms_exc, frame, ms_ctx, disp, pers);
#else
    // Foreign exception.
    // We can't interact with them (we don't know the original target frame
    // that we should pass on to RtlUnwindEx in _Unwind_Resume), so just
    // pass without calling our destructors here.
    return ExceptionContinueSearch;
#endif
  }
  if (!ctx) {
    __unw_init_seh(&cursor, disp->ContextRecord);
    __unw_seh_set_disp_ctx(&cursor, disp);
#if defined(_WIN32_ITANIUM)
    setPersonalityIP(&cursor, disp,
                     isSEHOrigin(exc) && exc->private_[4] == disp->ControlPc);
#else
    __unw_set_reg(&cursor, UNW_REG_IP, disp->ControlPc);
#endif
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
  // An SEH exception being unwound to a catch (...): frames the compiler
  // did not expect an exception to pass through get no cleanup call.
  if ((action & (_UA_CLEANUP_PHASE | _UA_HANDLER_FRAME)) == _UA_CLEANUP_PHASE &&
      isSEHOrigin(exc) &&
      !callSiteTableCovers(disp, disp->ControlPc -
                                     (exc->private_[4] == disp->ControlPc ? 0 : 1)))
    return ExceptionContinueSearch;
#endif
  _LIBUNWIND_TRACE_UNWINDING("_GCC_specific_handler() calling personality "
                             "function %p(1, %d, %llx, %p, %p)",
                             (void *)pers, action, exc->exception_class,
                             (void *)exc, (void *)ctx);
  urc = pers(1, action, exc->exception_class, exc, ctx);
  _LIBUNWIND_TRACE_UNWINDING("_GCC_specific_handler() personality returned %d", urc);
  switch (urc) {
  case _URC_CONTINUE_UNWIND:
    // If we're in phase 2, and the personality routine said to continue
    // at the target frame, we're in real trouble.
    if (action & _UA_HANDLER_FRAME)
      _LIBUNWIND_ABORT("Personality continued unwind at the target frame!");
    return ExceptionContinueSearch;
  case _URC_HANDLER_FOUND:
    // If we were called by __libunwind_seh_personality(), indicate that
    // a handler was found; otherwise, initiate phase 2 by unwinding.
    if (ours && ms_exc->NumberParameters == 3)
      return static_cast<EXCEPTION_DISPOSITION>(4);
    // This should never happen in phase 2.
    if (IS_UNWINDING(ms_exc->ExceptionFlags))
      _LIBUNWIND_ABORT("Personality indicated exception handler in phase 2!");
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
extern "C" _Unwind_Reason_Code
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
  int ms_act = static_cast<int>(
      disp_ctx->LanguageHandler(&ms_exc, (PVOID)disp_ctx->EstablisherFrame,
                                disp_ctx->ContextRecord, disp_ctx));
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

  EXCEPTION_RECORD rec;
#if defined(_WIN32_ITANIUM)
  if (isSEHException(exception_object)) {
    // A rethrown SEH exception: raise the original again. The object is not
    // reused; a handler that takes it gets a new one.
    SEHExceptionState *state = (SEHExceptionState *)exception_object;
    rec = state->record;
    if (state->unwind.exception_cleanup == sehHeapCleanup)
      free(state);
    RtlRaiseException(&rec);
    return _URC_END_OF_STACK;
  }
  if (isSEHOrigin(exception_object) && sehRethrow &&
      sehRethrow(exception_object, &rec)) {
    // The runtime's own object for an SEH exception, rethrown: the runtime
    // keeps the object and the structured exception is raised again.
    RtlRaiseException(&rec);
    return _URC_END_OF_STACK;
  }
#endif

  // Mark that this is a non-forced unwind, so _Unwind_Resume()
  // can do the right thing.
  memset(exception_object->private_, 0, sizeof(exception_object->private_));

  // phase 1: the search phase
  // We'll let the system do that for us. RtlRaiseException, unlike
  // RaiseException, records the exception address in this function rather
  // than in kernelbase.dll, so that an unhandled-exception filter can tell
  // an exception that no frame caught from one raised anywhere else.
  memset(&rec, 0, sizeof(rec));
  rec.ExceptionCode = STATUS_GCC_THROW;
  rec.NumberParameters = 1;
  rec.ExceptionInformation[0] = (ULONG_PTR)exception_object;
  RtlRaiseException(&rec);

  // If we get here, either something went horribly wrong or we reached the
  // top of the stack. Either way, let libc++abi call std::terminate().
  return _URC_END_OF_STACK;
}

#if defined(_WIN32_ITANIUM)
/// The functions that raise an exception's search. The start-up library's
/// unhandled-exception filter resumes a raise from one of them, so that the
/// thrower can call std::terminate; this names them when libunwind is linked
/// into the executable, which then exports neither.
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
#if defined(_WIN32_ITANIUM)
  } else if (isSEHException(exception_object) &&
             ((SEHExceptionState *)exception_object)->targetFrame != 0) {
    // A cleanup ran during an unwind that an __except or __finally frame
    // drives. Start that unwind again from this frame, with the same record
    // and return value, towards the same target.
    SEHExceptionState *state = (SEHExceptionState *)exception_object;
    EXCEPTION_RECORD ms_exc = state->record;
    CONTEXT ms_ctx;
    UNWIND_HISTORY_TABLE hist;
    memset(&hist, 0, sizeof(hist));
    RtlUnwindEx((PVOID)state->targetFrame, (PVOID)state->targetIp, &ms_exc,
                (PVOID)state->returnValue, &ms_ctx, &hist);
#endif
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
  uintptr_t result =
      (uintptr_t)__unw_seh_get_disp_ctx((unw_cursor_t *)context)->HandlerData;
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
