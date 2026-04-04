//===-- crt_begin_windows.c - Windows CRT section begin -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// CRT section sentinels, DSO handle, and floating-point marker for Windows.
// These symbols must exist at link time for any Windows Itanium module.
//
// The .CRT$X** sections are merged by the linker in alphabetical order:
//   .CRT$XIA -> .CRT$XIZ  : C initializers (int return, can fail)
//   .CRT$XCA -> .CRT$XCZ  : C++ constructors (void return)
//   .CRT$XPA -> .CRT$XPZ  : Pre-terminators
//   .CRT$XTA -> .CRT$XTZ  : Terminators
//
// This file provides the 'A' (begin) sentinels. See crt_end_windows.c for 'Z'.
//
//===----------------------------------------------------------------------===//

#if defined(LLVM_RUNTIME_WIN32)

// Function pointer types matching UCRT conventions.
typedef int (__cdecl *_PIFV)(void);   // C initializers
typedef void (__cdecl *_PVFV)(void);  // C++ constructors/destructors

#define WINCRT_SECTION(name) __attribute__((section(name), used))
#define WINCRT_SELECTANY __attribute__((selectany))

//===----------------------------------------------------------------------===//
// CRT section sentinels (begin)
//===----------------------------------------------------------------------===//

WINCRT_SECTION(".CRT$XIA") _PIFV __xi_a[] = {0};
WINCRT_SECTION(".CRT$XCA") _PVFV __xc_a[] = {0};
WINCRT_SECTION(".CRT$XPA") _PVFV __xp_a[] = {0};
WINCRT_SECTION(".CRT$XTA") _PVFV __xt_a[] = {0};

//===----------------------------------------------------------------------===//
// DSO handle (Itanium ABI)
//===----------------------------------------------------------------------===//
//
// Self-referential pointer used by __cxa_atexit to identify this module.
// Each shared library/executable has its own __dso_handle.
//

WINCRT_SELECTANY void *__dso_handle = &__dso_handle;

//===----------------------------------------------------------------------===//
// Floating-point marker
//===----------------------------------------------------------------------===//
//
// The linker looks for this symbol when floating-point code is present.
// Value 0x9875 matches MSVC convention.
//

WINCRT_SELECTANY int _fltused = 0x9875;

#endif // LLVM_RUNTIME_WIN32
