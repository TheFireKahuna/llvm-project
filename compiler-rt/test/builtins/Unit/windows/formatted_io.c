// RUN: %clang_wincrt -shared -DDLL %s -o %t.dll
// RUN: %clang_wincrt %s -o %t.exe
// RUN: %run %t.exe %t.dll | FileCheck %s --check-prefixes=CHECK,FORMAT
// RUN: llvm-readobj --coff-imports %t.exe | FileCheck %s --check-prefix=IMPORT
// RUN: %clang_wincrt -static -DSTATIC %s -o %t-static.exe
// RUN: %run %t-static.exe | FileCheck %s --check-prefix=FORMAT
// RUN: llvm-readobj --coff-imports %t-static.exe | FileCheck %s \
// RUN:     --check-prefix=STATIC-IMPORT

// The Universal CRT's formatted I/O, which its headers define inline and no
// DLL of it exports, is defined once for the program, by
// clang_rt.wincrt_dynamic.dll: every image's &printf is the DLL's export,
// which GetProcAddress finds, and the functions format through the Universal
// CRT's streams and locales. With -static they are the executable's own.

#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

typedef int (*PrintfType)(const char *, ...);

#ifdef DLL

__declspec(dllexport) PrintfType dllPrintf(void) { return printf; }

#else

static const PrintfType StaticPrintf = printf;

static int format(char *Buffer, size_t Size, const char *Format, ...) {
  va_list Args;
  va_start(Args, Format);
  int Result = vsnprintf(Buffer, Size, Format, Args);
  va_end(Args);
  return Result;
}

int main(int argc, char **argv) {
#ifndef STATIC
  HMODULE Module = LoadLibraryA(argv[1]);
  PrintfType (*DllPrintf)(void) =
      (PrintfType(*)(void))GetProcAddress(Module, "dllPrintf");
  HMODULE Runtime = GetModuleHandleW(L"clang_rt.wincrt_dynamic.dll");
  printf("dll %d, runtime %d, static %d\n", DllPrintf() == printf,
         (PrintfType)GetProcAddress(Runtime, "printf") == printf,
         StaticPrintf == printf);
#endif
  PrintfType volatile Indirect = printf;
  Indirect("indirect %d\n", 1);

  char Buffer[64];
  int Length = format(Buffer, 4, "%s %d", "truncated", 42);
  printf("vsnprintf %d \"%s\"\n", Length, Buffer);
  Length = snprintf(Buffer, sizeof(Buffer), "%5.2f|%-4x|%lld", 3.14159, 255,
                    -9000000000LL);
  printf("snprintf %d \"%s\"\n", Length, Buffer);
  printf("_scprintf %d\n", _scprintf("%08d", 7));

  _locale_t German = _create_locale(LC_NUMERIC, "de-DE");
  _sprintf_l(Buffer, "%.2f", German, 1.5);
  printf("_sprintf_l \"%s\"\n", Buffer);
  double Value = 0;
  _sscanf_l("2,25", "%lf", German, &Value);
  printf("_sscanf_l %.2f\n", Value);
  _free_locale(German);

  int Number = 0;
  char Word[16];
  int Fields = sscanf("17 apples", "%d %15s", &Number, Word);
  printf("sscanf %d %d %s\n", Fields, Number, Word);

  wchar_t Wide[32];
  swprintf(Wide, 32, L"%s/%ls/%d", "narrow", L"wide", 3);
  printf("swprintf %ls\n", Wide);
  wchar_t WideWord[16];
  swscanf(L"9 pears", L"%d %15ls", &Number, WideWord);
  printf("swscanf %d %ls\n", Number, WideWord);

  printf("msvcrt.dll %d, kernel.appcore.dll %d\n",
         GetModuleHandleW(L"msvcrt.dll") != NULL,
         GetModuleHandleW(L"kernel.appcore.dll") != NULL);
  return 0;
}

#endif

// CHECK:       dll 1, runtime 1, static 1
// FORMAT:      indirect 1
// FORMAT-NEXT: vsnprintf 12 "tru"
// FORMAT-NEXT: snprintf 22 " 3.14|ff  |-9000000000"
// FORMAT-NEXT: _scprintf 8
// FORMAT-NEXT: _sprintf_l "1,50"
// FORMAT-NEXT: _sscanf_l 2.25
// FORMAT-NEXT: sscanf 2 17 apples
// FORMAT-NEXT: swprintf narrow/wide/3
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
