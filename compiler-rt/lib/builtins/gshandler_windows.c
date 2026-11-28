//===-- gshandler_windows.c - What an MSVC /GS object names ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// An object that MSVC compiles with /GS names __GSHandlerCheck as the language
// handler of a frame that holds a stack cookie, so that a corrupted cookie is
// caught before an exception is dispatched through the frame or unwinds it,
// and on AArch64 calls __security_push_cookie and __security_pop_cookie.
// Clang emits none of them, but an image needs them to link such an object.
//
// The handler data is a 32-bit word: three flags, and in the other bits the
// cookie's offset from the establisher frame. Where the frame's locals are
// dynamically aligned, the offset is from an aligned base, and the base's
// offset from the establisher frame and the alignment follow the word.
//
//===----------------------------------------------------------------------===//

#include <stdint.h>

// The fields of DISPATCHER_CONTEXT that the check reads, where both machines
// have them.
typedef struct {
  uintptr_t ControlPc, ImageBase;
  const uint32_t *FunctionEntry;
  void *Unused[4];
  const int32_t *HandlerData;
} DispatcherContext;

// Every entry has the Windows convention, which NT-POSIX's C convention is
// not: the OS calls the handlers, and MSVC's objects the cookie check.
#define GS __attribute__((ms_abi, thread_pointer("none")))

__attribute__((ms_abi)) void __security_check_cookie(uintptr_t);

// The flag that the base is aligned, beside the two that
// __GSHandlerCheck_SEH reads.
enum { Aligned = 4, Flags = 7 };

GS void __GSHandlerCheckCommon(void *EstablisherFrame,
                               const DispatcherContext *Dispatch,
                               const int32_t *Data) {
  uintptr_t Frame = (uintptr_t)EstablisherFrame, Base = Frame;
  if (Data[0] & Aligned)
    Base = (Frame + Data[1]) & -(uintptr_t)Data[2];
  uintptr_t Slot = Base + (Data[0] & ~Flags);
#if defined(__x86_64__)
  // The frame stores the cookie XORed with its frame register, or with the
  // stack pointer after the prologue if it has none. The unwinder made the
  // establisher frame from the same byte of the unwind information: the
  // register in the low four bits and its offset, in units of 16, above them.
  uint8_t Register =
      ((const uint8_t *)(Dispatch->ImageBase + Dispatch->FunctionEntry[2]))[3];
  uintptr_t Key = Frame + (Register & 15 ? (Register >> 4) * 16 : 0);
#else
  // The establisher frame is the stack pointer at entry, so the offset is
  // negative, and __security_push_cookie stores the cookie XORed with its
  // slot's address.
  uintptr_t Key = Slot;
#endif
  __security_check_cookie(*(const uintptr_t *)Slot ^ Key);
}

GS int __GSHandlerCheck(void *Record, void *EstablisherFrame, void *Context,
                        const DispatcherContext *Dispatch) {
  __GSHandlerCheckCommon(EstablisherFrame, Dispatch, Dispatch->HandlerData);
  return 1; // ExceptionContinueSearch
}

// Called where an array index fails a /GS range check.
__attribute__((naked)) void __report_rangecheckfailure(void) {
#if defined(__x86_64__)
  __asm__("movl $8, %ecx\n" // FAST_FAIL_RANGE_CHECK_FAILURE
          "int $0x29");
#else
  __asm__("mov x0, #8\n"
          "brk #0xf003");
#endif
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
