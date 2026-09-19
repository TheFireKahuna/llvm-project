//===-- gshandler.cpp - /GS cookie check during exception dispatch --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// MSVC-built objects compiled with /GS register one of these routines as the
// language-specific handler of every frame that has an unwind handler, so
// that a corrupted cookie is caught before an exception unwinds past the
// frame. Clang never emits them, but any MSVC-built static library with a
// __try block, an alloca or C++ exception handling references them, and this
// image must resolve them to link that library.
//
// The handler data is MSVC's GS_HANDLER_DATA: one 32-bit word whose low three
// bits are flags and whose remaining bits give the cookie's offset from the
// establisher frame, followed by an aligned-base offset and an alignment when
// the flag says the frame is dynamically aligned. __GSHandlerCheck receives
// the word directly; __GSHandlerCheck_SEH receives a __C_specific_handler
// scope table with the word after its last record. The compiler XORs the
// cookie with RSP after the prologue, or with the frame register when one is
// established; the establisher frame the unwinder passes is that RSP, and the
// frame register is that value plus the scaled offset in the unwind
// information. The plain and frame-register forms were checked against
// MSVC-built objects; the dynamically aligned form follows the data layout.
//
// Anything inconsistent, a cookie outside the thread's stack, a chained
// unwind record that does not terminate, or an alignment that is not a power
// of two, is reported as a cookie failure rather than dereferenced.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

#if defined(__x86_64__)

// __C_specific_handler comes from excpt.h through windows.h.
extern uintptr_t __security_cookie;
extern "C" [[noreturn]] void __cdecl __report_gsfailure(uintptr_t);

namespace {

struct GSHandlerData {
  uint32_t CookieOffsetAndFlags;
  int32_t AlignedBaseOffset;
  int32_t Alignment;
};

// Flags in the low bits of CookieOffsetAndFlags.
constexpr uint32_t GSChainOnDispatch = 1; // call the language handler when
                                          // dispatching
constexpr uint32_t GSChainOnUnwind = 2;   // and when unwinding
constexpr uint32_t GSHasAlignment = 4;    // the offset is from an aligned base
constexpr uint32_t GSFlagMask = 7;

// The SDK declares no UNWIND_INFO; this is the documented x64 layout.
struct UnwindInfo {
  uint8_t VersionAndFlags;
  uint8_t SizeOfProlog;
  uint8_t CountOfCodes;
  uint8_t FrameRegisterAndOffset;
  uint16_t UnwindCode[1];
};

constexpr uint8_t UnwindFlagChainInfo = 4;
// MSVC chains at most one level; a longer chain is corruption.
constexpr unsigned MaxChainDepth = 4;

constexpr bool isPowerOfTwo(uintptr_t Value) {
  return Value != 0 && (Value & (Value - 1)) == 0;
}

// Follows chained unwind information to the primary entry, which is the one
// that names the frame register. Returns null when the chain does not end.
const UnwindInfo *primaryUnwindInfo(const DISPATCHER_CONTEXT *Dispatch) {
  const RUNTIME_FUNCTION *Entry = Dispatch->FunctionEntry;
  for (unsigned Depth = 0; Depth < MaxChainDepth; ++Depth) {
    const auto *Info = reinterpret_cast<const UnwindInfo *>(
        Dispatch->ImageBase + Entry->UnwindData);
    if (!((Info->VersionAndFlags >> 3) & UnwindFlagChainInfo))
      return Info;
    // The code array is padded to an even count; the chained entry follows.
    const unsigned Codes = (Info->CountOfCodes + 1u) & ~1u;
    Entry = reinterpret_cast<const RUNTIME_FUNCTION *>(
        reinterpret_cast<const uint8_t *>(Info) + offsetof(UnwindInfo, UnwindCode) +
        Codes * sizeof(uint16_t));
  }
  return nullptr;
}

bool onThisStack(uintptr_t Address, size_t Size) {
  const NT_TIB *Tib = reinterpret_cast<const NT_TIB *>(NtCurrentTeb());
  const auto Limit = reinterpret_cast<uintptr_t>(Tib->StackLimit);
  const auto Base = reinterpret_cast<uintptr_t>(Tib->StackBase);
  return Address >= Limit && Address <= Base - Size;
}

void checkCookie(uintptr_t EstablisherFrame,
                 const DISPATCHER_CONTEXT *Dispatch,
                 const GSHandlerData *Data) {
  uintptr_t Base = EstablisherFrame;
  uintptr_t Xor = EstablisherFrame;
  if (Data->CookieOffsetAndFlags & GSHasAlignment) {
    const auto Alignment = static_cast<uintptr_t>(Data->Alignment);
    if (!isPowerOfTwo(Alignment))
      __report_gsfailure(0);
    Base = (EstablisherFrame + Data->AlignedBaseOffset) & ~(Alignment - 1);
    Xor = Base;
  } else if (const UnwindInfo *Info = primaryUnwindInfo(Dispatch)) {
    if (Info->FrameRegisterAndOffset & 0xF)
      Xor += static_cast<uintptr_t>(Info->FrameRegisterAndOffset >> 4) * 16;
  } else {
    __report_gsfailure(0);
  }

  const uintptr_t CookieAddress =
      Base + (Data->CookieOffsetAndFlags & ~GSFlagMask);
  if (!onThisStack(CookieAddress, sizeof(uintptr_t)))
    __report_gsfailure(0);

  const uintptr_t Cookie =
      *reinterpret_cast<const uintptr_t *>(CookieAddress) ^ Xor;
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
      reinterpret_cast<const uint8_t *>(Scopes) +
      offsetof(SCOPE_TABLE, ScopeRecord) +
      static_cast<size_t>(Scopes->Count) * sizeof(Scopes->ScopeRecord[0]));
  checkCookie(reinterpret_cast<uintptr_t>(EstablisherFrame), Dispatch, Data);
  const uint32_t Wanted = (Record->ExceptionFlags & EXCEPTION_UNWIND)
                              ? GSChainOnUnwind
                              : GSChainOnDispatch;
  if (Data->CookieOffsetAndFlags & Wanted)
    return __C_specific_handler(Record, EstablisherFrame, Context, Dispatch);
  return ExceptionContinueSearch;
}

// Emitted by MSVC for a failed compile-time array bounds check under /GS.
[[noreturn]] void __cdecl __report_rangecheckfailure(void) {
  __fastfail(FAST_FAIL_RANGE_CHECK_FAILURE);
}

} // extern "C"

#endif // __x86_64__
