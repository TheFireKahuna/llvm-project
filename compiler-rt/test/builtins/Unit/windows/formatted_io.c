// RUN: %clang %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s --check-prefixes=CHECK,FORMAT
// RUN: llvm-readobj --coff-imports %t.exe | FileCheck %s --check-prefix=IMPORT
// RUN: %clang -static %s -o %t-static.exe 2>&1 | count 0
// RUN: %run %t-static.exe | FileCheck %s --check-prefixes=STATIC,FORMAT
// RUN: llvm-readobj --coff-imports %t-static.exe | FileCheck %s --check-prefix=STATIC-IMPORT
// RUN: %clang -DDECLARED %s -o %t-declared.exe
// RUN: %run %t-declared.exe | FileCheck %s --check-prefix=CHECK
// RUN: %clang -static -DDECLARED %s -o %t-declared-static.exe
// RUN: %run %t-declared-static.exe | FileCheck %s --check-prefix=STATIC

// The Universal CRT's formatted I/O, which its headers define inline and no
// DLL of it exports, is defined once for the program, by
// clang_rt.wincrt_dynamic.dll, which exports each function under its name,
// where a search of the process's modules by name, as a JIT's symbol lookup
// does, finds it. The functions format through the Universal CRT's streams
// and locales. With -static they are the executable's own, which exports
// nothing. [DECLARED] A C unit that declares printf itself, without the
// Universal CRT's headers, links and runs.

// CHECK:       exported 1
// STATIC:      exported 0
// FORMAT:      vsnprintf 12 "tru"
// FORMAT-NEXT: _sprintf_l "1,50"
// FORMAT-NEXT: swscanf 9 pears
// FORMAT-NEXT: msvcrt.dll 0, kernel.appcore.dll 0

// IMPORT:      Name: clang_rt.wincrt_dynamic.dll
// IMPORT-DAG:  Symbol: printf
// IMPORT-DAG:  Symbol: vsnprintf
// IMPORT-DAG:  Symbol: _sprintf_l
// IMPORT-DAG:  Symbol: swscanf
// IMPORT:      }

// STATIC-IMPORT-NOT: wincrt_dynamic
// STATIC-IMPORT-NOT: Symbol: printf

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

#ifdef DECLARED
int printf(const char *, ...);
#else
#  include <locale.h>
#  include <stdarg.h>
#  include <stdio.h>
#  include <wchar.h>

static int format(char *Buffer, size_t Size, const char *Format, ...) {
  va_list Args;
  va_start(Args, Format);
  int Result = vsnprintf(Buffer, Size, Format, Args);
  va_end(Args);
  return Result;
}
#endif

int main(void) {
  HMODULE Modules[256];
  DWORD Bytes = 0;
  int Exported = 0;
  K32EnumProcessModules(GetCurrentProcess(), Modules, sizeof(Modules), &Bytes);
  for (DWORD I = 0; I < Bytes / sizeof(HMODULE) && I < 256; ++I)
    Exported |= GetProcAddress(Modules[I], "printf") != NULL;
  int (*volatile Indirect)(const char *, ...) = printf;
  Indirect("exported %d\n", Exported);
#ifndef DECLARED
  char Buffer[64];
  printf("vsnprintf %d \"%s\"\n", format(Buffer, 4, "%s %d", "truncated", 42), Buffer);
  _locale_t German = _create_locale(LC_NUMERIC, "de-DE");
  _sprintf_l(Buffer, "%.2f", German, 1.5);
  printf("_sprintf_l \"%s\"\n", Buffer);
  _free_locale(German);
  int Number = 0;
  wchar_t Word[16];
  swscanf(L"9 pears", L"%d %15ls", &Number, Word);
  printf("swscanf %d %ls\n", Number, Word);
  printf("msvcrt.dll %d, kernel.appcore.dll %d\n", GetModuleHandleW(L"msvcrt.dll") != NULL,
         GetModuleHandleW(L"kernel.appcore.dll") != NULL);
#endif
  return 0;
}
