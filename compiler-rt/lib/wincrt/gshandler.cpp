//===-- gshandler.cpp - /GS cookie check during exception dispatch --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// An object that MSVC compiles with /GS names __GSHandlerCheck as the
// language-specific handler of a frame that holds a stack cookie, or
// __GSHandlerCheck_SEH when the frame also has __try scopes, so that a
// corrupted cookie is caught before an exception is dispatched through the
// frame or unwinds it. Clang never emits either, but an image needs them to
// link such an object.
//
// The handler data is a 32-bit word whose low three bits are flags and whose
// other bits are the cookie's offset from the establisher frame. When the
// frame's locals are dynamically aligned, the offset is from an aligned base
// instead, and the word is followed by the base's offset from the establisher
// frame and the alignment. For __GSHandlerCheck_SEH the data is the
// __C_specific_handler scope table, and the word follows its last record.
//
// On x86-64 the frame stores the cookie XORed with the stack pointer after the
// prologue, which is the establisher frame, or with the frame register when
// the frame has one, which is the establisher frame plus the offset its unwind
// information records. The aligned base only locates the cookie. On AArch64
// the establisher frame is the stack pointer at entry, so the offset is
// negative, and the frame stores the cookie through __security_push_cookie
// (gs_cookie.S), XORed with the address of its slot.
//
// A cookie outside the thread's stack, a chain of unwind entries that does not
// end, or an alignment that is not a power of two is reported as a cookie
// failure rather than dereferenced.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

extern "C" {
[[noreturn]] void __cdecl __report_gsfailure(uintptr_t);
}

namespace {

struct GSHandlerData {
  uint32_t CookieOffsetAndFlags;
  int32_t AlignedBaseOffset;
  int32_t Alignment;
};

// Flags in the low bits of CookieOffsetAndFlags: whether
// __GSHandlerCheck_SEH calls __C_specific_handler while dispatching and while
// unwinding, and whether the cookie offset is from an aligned base.
constexpr uint32_t GSChainOnDispatch = 1;
constexpr uint32_t GSChainOnUnwind = 2;
constexpr uint32_t GSHasAlignment = 4;
constexpr uint32_t GSFlagMask = 7;

#if defined(__x86_64__)
// The SDK declares no UNWIND_INFO; this is the documented x64 layout.
struct UnwindInfo {
  uint8_t VersionAndFlags;
  uint8_t SizeOfProlog;
  uint8_t CountOfCodes;
  uint8_t FrameRegisterAndOffset;
};

constexpr uint8_t UnwindFlagChainInfo = 4;
// Compilers chain an entry to its primary one directly; a longer chain is
// corrupt data.
constexpr unsigned MaxChainDepth = 32;

// The frame register's offset from the establisher frame, from the primary
// unwind entry of the function, the one that describes its prologue.
bool frameRegisterOffset(const DISPATCHER_CONTEXT *Dispatch,
                         uintptr_t &Offset) {
  const RUNTIME_FUNCTION *Entry = Dispatch->FunctionEntry;
  for (unsigned Depth = 0; Depth != MaxChainDepth; ++Depth) {
    const auto *Info = reinterpret_cast<const UnwindInfo *>(
        Dispatch->ImageBase + Entry->UnwindData);
    if (!(Info->VersionAndFlags >> 3 & UnwindFlagChainInfo)) {
      Offset = Info->FrameRegisterAndOffset & 0xF
                   ? (Info->FrameRegisterAndOffset >> 4) * 16u
                   : 0;
      return true;
    }
    // The chained entry follows the unwind codes, which are padded to an even
    // count.
    const unsigned Codes = (Info->CountOfCodes + 1u) & ~1u;
    Entry = reinterpret_cast<const RUNTIME_FUNCTION *>(
        reinterpret_cast<const uint8_t *>(Info + 1) + Codes * sizeof(uint16_t));
  }
  return false;
}
#endif

bool onThisStack(uintptr_t Address, size_t Size) {
  const auto *Tib = reinterpret_cast<const NT_TIB *>(NtCurrentTeb());
  const auto Limit = reinterpret_cast<uintptr_t>(Tib->StackLimit);
  const auto Base = reinterpret_cast<uintptr_t>(Tib->StackBase);
  return Address >= Limit && Address <= Base - Size;
}

void checkCookie(uintptr_t EstablisherFrame, const DISPATCHER_CONTEXT *Dispatch,
                 const GSHandlerData *Data) {
  uintptr_t Base = EstablisherFrame;
  if (Data->CookieOffsetAndFlags & GSHasAlignment) {
    const auto Alignment =
        static_cast<uintptr_t>(static_cast<uint32_t>(Data->Alignment));
    if (Alignment == 0 || (Alignment & (Alignment - 1)))
      __report_gsfailure(0);
    Base = (EstablisherFrame + Data->AlignedBaseOffset) & ~(Alignment - 1);
  }
  const uintptr_t Address =
      Base + static_cast<int32_t>(Data->CookieOffsetAndFlags & ~GSFlagMask);
  if (!onThisStack(Address, sizeof(uintptr_t)))
    __report_gsfailure(0);

#if defined(__x86_64__)
  uintptr_t FrameOffset;
  if (!frameRegisterOffset(Dispatch, FrameOffset))
    __report_gsfailure(0);
  const uintptr_t Mix = EstablisherFrame + FrameOffset;
#else
  (void)Dispatch;
  const uintptr_t Mix = Address;
#endif

  const uintptr_t Cookie = *reinterpret_cast<const uintptr_t *>(Address) ^ Mix;
  if (Cookie != __security_cookie)
    __report_gsfailure(Cookie);
}

} // namespace

extern "C" {

EXCEPTION_DISPOSITION __GSHandlerCheck(PEXCEPTION_RECORD,
                                       void *EstablisherFrame, PCONTEXT,
                                       PDISPATCHER_CONTEXT Dispatch) {
  checkCookie(reinterpret_cast<uintptr_t>(EstablisherFrame), Dispatch,
              static_cast<const GSHandlerData *>(Dispatch->HandlerData));
  return ExceptionContinueSearch;
}

EXCEPTION_DISPOSITION __GSHandlerCheck_SEH(PEXCEPTION_RECORD Record,
                                           void *EstablisherFrame,
                                           PCONTEXT Context,
                                           PDISPATCHER_CONTEXT Dispatch) {
  const auto *Scopes = static_cast<const SCOPE_TABLE *>(Dispatch->HandlerData);
  const auto *Data = reinterpret_cast<const GSHandlerData *>(
      &Scopes->ScopeRecord[Scopes->Count]);
  checkCookie(reinterpret_cast<uintptr_t>(EstablisherFrame), Dispatch, Data);
  const uint32_t Chain = Record->ExceptionFlags & EXCEPTION_UNWIND
                             ? GSChainOnUnwind
                             : GSChainOnDispatch;
  if (Data->CookieOffsetAndFlags & Chain)
    return __C_specific_handler(Record, EstablisherFrame, Context, Dispatch);
  return ExceptionContinueSearch;
}

// Called by MSVC-built code whose array index fails a /GS range check.
[[noreturn]] void __cdecl __report_rangecheckfailure(void) {
  __fastfail(FAST_FAIL_RANGE_CHECK_FAILURE);
}

} // extern "C"
