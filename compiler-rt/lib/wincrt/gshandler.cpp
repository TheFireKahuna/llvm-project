//===-- gshandler.cpp - What an MSVC /GS object names ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// An object that MSVC compiles with /GS names __GSHandlerCheck as the language
// handler of a frame that holds a stack cookie, or __GSHandlerCheck_SEH when
// the frame also has __try scopes, so that a corrupted cookie is caught before
// an exception is dispatched through the frame or unwinds it. Clang emits
// neither, but an image needs them to link such an object.
//
// The handler data is a 32-bit word: three flags, and in the other bits the
// cookie's offset from the establisher frame. Where the frame's locals are
// dynamically aligned, the offset is from an aligned base, and the base's
// offset from the establisher frame and the alignment follow the word. For
// __GSHandlerCheck_SEH the word follows __C_specific_handler's scope table.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

extern "C" void __cdecl __security_check_cookie(uintptr_t);

// The flags: whether __GSHandlerCheck_SEH goes on to __C_specific_handler
// while dispatching and while unwinding, and whether the base is aligned.
static constexpr int32_t ChainOnDispatch = 1, ChainOnUnwind = 2, Aligned = 4,
                         Flags = 7;

static void checkCookie(void *EstablisherFrame,
                        const DISPATCHER_CONTEXT *Dispatch,
                        const int32_t *Data) {
  auto Frame = reinterpret_cast<uintptr_t>(EstablisherFrame), Base = Frame;
  if (Data[0] & Aligned)
    Base = (Frame + Data[1]) & -static_cast<uintptr_t>(Data[2]);
  uintptr_t Slot = Base + (Data[0] & ~Flags);
#if defined(__x86_64__)
  // The frame stores the cookie XORed with its frame register, or with the
  // stack pointer after the prologue if it has none. The unwinder made the
  // establisher frame from the same byte of the unwind information: the
  // register in the low four bits and its offset, in units of 16, above them.
  uint8_t Register = reinterpret_cast<const uint8_t *>(
      Dispatch->ImageBase + Dispatch->FunctionEntry->UnwindData)[3];
  uintptr_t Key = Frame + (Register & 15 ? (Register >> 4) * 16 : 0);
#else
  // The establisher frame is the stack pointer at entry, so the offset is
  // negative, and __security_push_cookie stores the cookie XORed with its
  // slot's address.
  uintptr_t Key = Slot;
#endif
  __security_check_cookie(*reinterpret_cast<const uintptr_t *>(Slot) ^ Key);
}

extern "C" {

EXCEPTION_DISPOSITION __GSHandlerCheck(PEXCEPTION_RECORD,
                                       void *EstablisherFrame, PCONTEXT,
                                       PDISPATCHER_CONTEXT Dispatch) {
  checkCookie(EstablisherFrame, Dispatch,
              static_cast<const int32_t *>(Dispatch->HandlerData));
  return ExceptionContinueSearch;
}

EXCEPTION_DISPOSITION __GSHandlerCheck_SEH(PEXCEPTION_RECORD Record,
                                           void *EstablisherFrame,
                                           PCONTEXT Context,
                                           PDISPATCHER_CONTEXT Dispatch) {
  auto *Scopes = static_cast<const SCOPE_TABLE *>(Dispatch->HandlerData);
  auto *Data =
      reinterpret_cast<const int32_t *>(&Scopes->ScopeRecord[Scopes->Count]);
  checkCookie(EstablisherFrame, Dispatch, Data);
  if (*Data & (Record->ExceptionFlags & EXCEPTION_UNWIND ? ChainOnUnwind
                                                         : ChainOnDispatch))
    return __C_specific_handler(Record, EstablisherFrame, Context, Dispatch);
  return ExceptionContinueSearch;
}

// Called where an array index fails a /GS range check.
[[noreturn]] void __cdecl __report_rangecheckfailure(void) {
  __fastfail(FAST_FAIL_RANGE_CHECK_FAILURE);
}

#if defined(__aarch64__)
// An AArch64 /GS function calls the first in its prologue and the second
// before its epilogue. The push takes 16 bytes of the caller's stack, which
// the caller's unwind codes record, and stores the cookie in the upper 8. The
// caller keeps its arguments and its return value in registers across both,
// so they change only X16, X17 and the flags.
__attribute__((naked)) void __security_push_cookie(void) {
  __asm__("sub sp, sp, #16\n"
          "adrp x16, __security_cookie\n"
          "ldr x16, [x16, :lo12:__security_cookie]\n"
          "add x17, sp, #8\n"
          "eor x16, x16, x17\n"
          "str x16, [sp, #8]\n"
          "ret");
}

__attribute__((naked)) void __security_pop_cookie(void) {
  __asm__("add x17, sp, #8\n"
          "ldr x16, [x17]\n"
          "eor x16, x16, x17\n"
          "adrp x17, __security_cookie\n"
          "ldr x17, [x17, :lo12:__security_cookie]\n"
          "cmp x16, x17\n"
          "b.ne 1f\n"
          "add sp, sp, #16\n"
          "ret\n"
          "1: b __stack_chk_fail");
}
#endif

} // extern "C"
