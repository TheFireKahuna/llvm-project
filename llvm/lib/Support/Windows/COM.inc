//==- llvm/Support/Windows/COM.inc - Windows COM Implementation -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the Windows portion of COM support.
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
//=== WARNING: Implementation here must contain only Windows code.
//===----------------------------------------------------------------------===//

// When building against LLVM libc (POSIX-on-NT), declare the COM APIs inline
// so that <objbase.h> and its transitive MSVC SDK headers are never pulled in.
#if defined(LLVM_RUNTIME_POSIX)
#include <stdint.h>
typedef unsigned long DWORD;
typedef long HRESULT;
typedef void *PVOID;
#define WINAPI __stdcall
#ifndef COINIT_APARTMENTTHREADED
#define COINIT_APARTMENTTHREADED 0x2
#define COINIT_MULTITHREADED     0x0
#define COINIT_SPEED_OVER_MEMORY 0x8
#endif
extern "C" {
__declspec(dllimport) HRESULT WINAPI CoInitializeEx(PVOID, DWORD);
__declspec(dllimport) void WINAPI CoUninitialize(void);
}
#else
#include <objbase.h>
#endif

namespace llvm {
namespace sys {

InitializeCOMRAII::InitializeCOMRAII(COMThreadingMode Threading,
                                     bool SpeedOverMemory) {
  DWORD Coinit = 0;
  if (Threading == COMThreadingMode::SingleThreaded)
    Coinit |= COINIT_APARTMENTTHREADED;
  else
    Coinit |= COINIT_MULTITHREADED;
  if (SpeedOverMemory)
    Coinit |= COINIT_SPEED_OVER_MEMORY;
  ::CoInitializeEx(nullptr, Coinit);
}

InitializeCOMRAII::~InitializeCOMRAII() { ::CoUninitialize(); }
} // namespace sys
} // namespace llvm
