//===-- crt_windows.c - CRT sections and markers for COFF images ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Defines the symbols that the MSVC C runtime's startup objects would
// otherwise provide to a COFF image: the bounds of the .CRT$X?? initializer
// and terminator tables, __dso_handle and _fltused.
//
// The linker sorts the .CRT$X?? sections of all inputs by name, so the
// entries the compiler emits into .CRT$XCU and the like land between the
// "A" and "Z" sentinels below, whichever object defines them.
//
//===----------------------------------------------------------------------===//

typedef int (*_PIFV)(void);
typedef void (*_PVFV)(void);

// C initializers, which return nonzero to fail start-up.
__attribute__((section(".CRT$XIA"))) const _PIFV __xi_a[] = {0};
__attribute__((section(".CRT$XIZ"))) const _PIFV __xi_z[] = {0};
// C++ constructors.
__attribute__((section(".CRT$XCA"))) const _PVFV __xc_a[] = {0};
__attribute__((section(".CRT$XCZ"))) const _PVFV __xc_z[] = {0};
// Pre-terminators.
__attribute__((section(".CRT$XPA"))) const _PVFV __xp_a[] = {0};
__attribute__((section(".CRT$XPZ"))) const _PVFV __xp_z[] = {0};
// Terminators.
__attribute__((section(".CRT$XTA"))) const _PVFV __xt_a[] = {0};
__attribute__((section(".CRT$XTZ"))) const _PVFV __xt_z[] = {0};

// Identifies this image to __cxa_atexit and __cxa_finalize. Every image links
// the builtins statically, so each gets its own.
void *__dso_handle = &__dso_handle;

// Objects compiled by MSVC reference this when they use floating point; the
// value is the one the MSVC runtime gives it.
int _fltused = 0x9875;
