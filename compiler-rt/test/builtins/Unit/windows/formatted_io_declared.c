// RUN: %clang_wincrt %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
// RUN: %clang_wincrt -static %s -o %t-static.exe
// RUN: %run %t-static.exe | FileCheck %s --check-prefix=STATIC

// A C unit that declares printf itself, without the Universal CRT's headers,
// links and runs. A search of the process's modules by name, as a JIT's
// symbol lookup does, finds printf in clang_rt.wincrt_dynamic.dll, the
// definition the program's own references bind to.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

int printf(const char *, ...);
int snprintf(char *, __SIZE_TYPE__, const char *, ...);

typedef int (*PrintfType)(const char *, ...);

static PrintfType searchProcess(const char *Name) {
  HMODULE Modules[256];
  DWORD Bytes;
  if (!K32EnumProcessModules(GetCurrentProcess(), Modules, sizeof(Modules),
                             &Bytes))
    return NULL;
  for (DWORD I = 0; I < Bytes / sizeof(HMODULE) && I < 256; ++I) {
    FARPROC Found = GetProcAddress(Modules[I], Name);
    if (Found)
      return (PrintfType)Found;
  }
  return NULL;
}

int main(void) {
  char Buffer[16];
  snprintf(Buffer, sizeof(Buffer), "%d-%s", 12, "ab");
  printf("declared %s\n", Buffer);
  printf("search %d\n", searchProcess("printf") == printf);
  return 0;
}

// CHECK:      declared 12-ab
// CHECK-NEXT: search 1

// A -static program defines printf itself and exports nothing.
// STATIC:      declared 12-ab
// STATIC-NEXT: search 0
