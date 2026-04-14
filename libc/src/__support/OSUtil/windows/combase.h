//===-- COM base API declarations for Windows -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// COM apartment initialization via combase.dll, without the Windows SDK.
//
// CoInitializeEx / CoUninitialize are the only COM APIs needed by LLVM tools
// (llvm-symbolizer, llvm-pdbutil, MSVCPaths.cpp). Everything else is either
// a COM interface call or a no-op on non-Windows.
//
// Constants verified against combase.dll disassembly:
//   - COINIT_MULTITHREADED  : input flag 0x0  (OLETLS_MULTITHREADED = 0x100 in TEB)
//   - COINIT_APARTMENTTHREADED: input flag 0x2 (OLETLS_APARTMENTTHREADED = 0x80 in TEB)
//   - E_INVALIDARG returned for bad flag combos (0x80070057, confirmed)
//   - RPC_E_CHANGED_MODE returned on mode conflict (0x80010106, confirmed)
//
// The apartment object (CComApartment) at SOleTlsData+0x170 and the full
// SOleTlsData (0x248 bytes) are managed entirely inside combase.dll.
// We cannot replicate that logic, so we always call through to combase.dll.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_COMBASE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_COMBASE_H

#include "src/__support/OSUtil/windows/nt/nt_types.h"

//===----------------------------------------------------------------------===//
// COINIT flags (input to CoInitializeEx)
//===----------------------------------------------------------------------===//

// SDK defines these inside a tagCOINIT enum with no header guard; guard on the
// first value so the block is skipped cleanly if objbase.h was included first.
#ifndef COINIT_APARTMENTTHREADED
// Threading model — bits 0-3. Exactly one must be set (MULTITHREADED = 0).
inline constexpr DWORD COINIT_APARTMENTTHREADED = 0x2; // STA
inline constexpr DWORD COINIT_MULTITHREADED     = 0x0; // MTA (default)

// Optional modifier — may combine with either threading model.
inline constexpr DWORD COINIT_DISABLE_OLE1DDE   = 0x4;
inline constexpr DWORD COINIT_SPEED_OVER_MEMORY = 0x8;
#endif // COINIT_APARTMENTTHREADED

//===----------------------------------------------------------------------===//
// COM HRESULT codes
//===----------------------------------------------------------------------===//

// HRESULT is defined as `typedef long HRESULT` in ntabi.h (included via
// nt_types.h). S_OK / S_FALSE also live there now; no local redefinition
// needed here.
#ifndef RPC_E_CHANGED_MODE
// Returned when CoInitializeEx is called with a conflicting apartment model.
inline constexpr HRESULT RPC_E_CHANGED_MODE = static_cast<HRESULT>(0x80010106);
#endif

//===----------------------------------------------------------------------===//
// API declarations (combase.dll)
//===----------------------------------------------------------------------===//

extern "C" {

// Initialize COM on the current thread. Must be balanced by CoUninitialize.
// pvReserved must be nullptr. dwCoInit is a combination of COINIT_* flags.
__declspec(dllimport) HRESULT WINAPI CoInitializeEx(PVOID pvReserved,
                                                    DWORD dwCoInit);

// Decrement the COM init count for the current thread. When the count reaches
// zero the apartment is torn down and TEB->ReservedForOle is freed.
__declspec(dllimport) void WINAPI CoUninitialize(void);

} // extern "C"

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_COMBASE_H
