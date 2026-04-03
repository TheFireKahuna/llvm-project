//===-- crt_end_windows.c - Windows CRT section end -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// CRT section end sentinels for Windows.
// See crt_begin_windows.c for documentation.
//
//===----------------------------------------------------------------------===//

#ifdef _WIN32

// Function pointer types matching UCRT conventions.
typedef int (__cdecl *_PIFV)(void);   // C initializers
typedef void (__cdecl *_PVFV)(void);  // C++ constructors/destructors

#define WINCRT_SECTION(name) __attribute__((section(name), used))

//===----------------------------------------------------------------------===//
// CRT section sentinels (end)
//===----------------------------------------------------------------------===//

WINCRT_SECTION(".CRT$XIZ") _PIFV __xi_z[] = {0};
WINCRT_SECTION(".CRT$XCZ") _PVFV __xc_z[] = {0};
WINCRT_SECTION(".CRT$XPZ") _PVFV __xp_z[] = {0};
WINCRT_SECTION(".CRT$XTZ") _PVFV __xt_z[] = {0};

#endif // _WIN32
