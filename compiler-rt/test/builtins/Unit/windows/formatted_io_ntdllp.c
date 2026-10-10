// REQUIRES: ntdllp
// RUN: %clang_wincrt -shared -DDLL %s -lntdllp -o %t.dll
// RUN: %clang_wincrt %s -lntdllp -o %t.exe
// RUN: %run %t.exe %t.dll | FileCheck %s --check-prefixes=CHECK,FORMAT
// RUN: llvm-readobj --coff-imports %t.exe | FileCheck %s --check-prefix=RUNTIME
// RUN: llvm-readobj --coff-imports %t.exe | FileCheck %s --check-prefix=NTDLL
// RUN: llvm-readobj --coff-imports %t.dll | FileCheck %s \
// RUN:     --check-prefix=DLL-NTDLL
// RUN: %clang_wincrt -static -DSTATIC %s -lntdllp -o %t-static.exe
// RUN: %run %t-static.exe | FileCheck %s --check-prefix=FORMAT
// RUN: llvm-readobj --coff-imports %t-static.exe | FileCheck %s \
// RUN:     --check-prefix=NTDLL

// ntdll.dll exports reduced forms of some of the Universal CRT's formatted
// I/O, which the Windows SDK's private ntdllp.lib imports. A program that
// links it still calls the runtime's definitions, which format floating
// point, and has one &sprintf, which GetProcAddress finds by its name.

#include <stdio.h>
#include <wchar.h>
#include <windows.h>

typedef int (*SprintfType)(char *, const char *, ...);

#ifdef DLL

__declspec(dllexport) SprintfType dllSprintf(void) { return sprintf; }

#else

static const SprintfType StaticSprintf = sprintf;

int main(int argc, char **argv) {
#ifndef STATIC
  HMODULE Module = LoadLibraryA(argv[1]);
  SprintfType (*DllSprintf)(void) =
      (SprintfType(*)(void))GetProcAddress(Module, "dllSprintf");
  HMODULE Runtime = GetModuleHandleW(L"clang_rt.wincrt_dynamic.dll");
  printf("dll %d, runtime %d, static %d\n", DllSprintf() == sprintf,
         (SprintfType)(void *)GetProcAddress(Runtime, "sprintf") == sprintf,
         StaticSprintf == sprintf);
#endif
  SprintfType volatile Indirect = sprintf;
  char Buffer[64];
  Indirect(Buffer, "%.2f", 1.25);
  printf("indirect %s\n", Buffer);

  sprintf(Buffer, "%.1f|%s", 2.5, "a");
  printf("sprintf %s\n", Buffer);
  _snprintf(Buffer, sizeof(Buffer), "%e", 1000.0);
  printf("_snprintf %s\n", Buffer);
  sprintf_s(Buffer, sizeof(Buffer), "%g", 0.5);
  printf("sprintf_s %s\n", Buffer);
  double Value = 0;
  sscanf("3.75", "%lf", &Value);
  printf("sscanf %.2f\n", Value);
  wchar_t Wide[32];
  swprintf(Wide, 32, L"%.1f", 4.5);
  printf("swprintf %ls\n", Wide);
  return 0;
}

#endif

// CHECK:       dll 1, runtime 1, static 1
// FORMAT:      indirect 1.25
// FORMAT-NEXT: sprintf 2.5|a
// FORMAT-NEXT: _snprintf 1.000000e+03
// FORMAT-NEXT: sprintf_s 0.5
// FORMAT-NEXT: sscanf 3.75
// FORMAT-NEXT: swprintf 4.5

// RUNTIME:     Name: clang_rt.wincrt_dynamic.dll
// RUNTIME-DAG: Symbol: sprintf (
// RUNTIME-DAG: Symbol: _snprintf (
// RUNTIME-DAG: Symbol: sprintf_s (
// RUNTIME-DAG: Symbol: sscanf (
// RUNTIME-DAG: Symbol: swprintf (
// RUNTIME:     }

// NTDLL:     Name: ntdll.dll
// NTDLL-NOT: {{printf|scanf}}
// NTDLL:     }

// The DLL imports nothing from ntdll.dll, its other ntdllp.lib names included.
// DLL-NTDLL-NOT: Name: ntdll.dll
