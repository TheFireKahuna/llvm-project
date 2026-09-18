//===- llvm/Support/Unix/COM.inc - Unix COM Implementation -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the Unix portion of COM support.
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
//=== WARNING: Implementation here must contain only generic UNIX code that
//===          is guaranteed to work on *all* UNIX variants.
//===----------------------------------------------------------------------===//

#if defined(LLVM_RUNTIME_NTPOSIX)
// combase is present; declare the two entry points so no SDK header is
// needed. The types come from <sys/ntabi.h> through WindowsSupport.h.
#include "llvm/Support/Windows/WindowsSupport.h"

extern "C" {
__declspec(dllimport) HRESULT NTAPI CoInitializeEx(void *, DWORD);
__declspec(dllimport) void NTAPI CoUninitialize(void);
}

namespace llvm {
namespace sys {
InitializeCOMRAII::InitializeCOMRAII(COMThreadingMode Threading,
                                     bool SpeedOverMemory) {
  DWORD Init = Threading == COMThreadingMode::SingleThreaded
                   ? 0x2   // COINIT_APARTMENTTHREADED
                   : 0x0;  // COINIT_MULTITHREADED
  if (SpeedOverMemory)
    Init |= 0x8; // COINIT_SPEED_OVER_MEMORY
  ::CoInitializeEx(nullptr, Init);
}

InitializeCOMRAII::~InitializeCOMRAII() { ::CoUninitialize(); }
} // namespace sys
} // namespace llvm
#else
namespace llvm {
namespace sys {

InitializeCOMRAII::InitializeCOMRAII(COMThreadingMode Threading,
                                     bool SpeedOverMemory) {}

InitializeCOMRAII::~InitializeCOMRAII() = default;
} // namespace sys
} // namespace llvm
#endif
