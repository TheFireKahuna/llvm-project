// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -fms-extensions -std=c++20 -fsyntax-only -verify %s

// expected-no-diagnostics

// A Win32 region declares a builtin with its own types where the builtin has
// their standard twins, and with Windows's convention spellings, as excpt.h
// declares _exception_code: unsigned long, which the builtin spells unsigned
// int on LP64, and __cdecl, which is MS x64 here.
#pragma clang win32_header

unsigned long __cdecl _exception_code(void);
void *__cdecl _exception_info(void);
