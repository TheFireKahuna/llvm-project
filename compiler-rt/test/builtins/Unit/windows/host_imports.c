// RUN: rm -rf %t && mkdir %t
// RUN: %clang_wincrt -DHOST=1 %s -o %t/host.exe
// RUN: %clang_wincrt -DHOST=2 %s -o %t/other.exe
// RUN: %clang_wincrt -shared -DWEAKDEP %s -o %t/weakdep.dll
// RUN: %clang_wincrt -shared -DPLUGIN %s %t/host.lib %t/weakdep.dll.lib \
// RUN:   -o %t/plugin.dll
// RUN: llvm-readobj --coff-imports %t/plugin.dll | FileCheck --check-prefix=IMPORTS %s
// RUN: %run %t/host.exe %t/plugin.dll | FileCheck --check-prefix=ONE %s
// RUN: %run %t/other.exe %t/plugin.dll | FileCheck --check-prefix=TWO %s
// RUN: cp %t/host.exe %t/renamed.exe
// RUN: %run %t/renamed.exe %t/plugin.dll | FileCheck --check-prefix=ONE %s
// RUN: %run %t/host.exe %t/plugin.dll %t/weakdep.dll | FileCheck --check-prefix=WEAK %s

// A plugin's imports from its host bind to the executable that loads it,
// whatever its file name: the same plugin reads each of two hosts' state, and
// a renamed host beside its original binds the running copy, not the file the
// plugin was linked against. A weak import from a DLL that is not loaded is
// null, and the DLL is not a dependency; once the DLL is loaded, it binds.

// IMPORTS-NOT: Name: host.exe
// IMPORTS-NOT: Name: weakdep.dll

// ONE: host 1 value 10 weak null
// TWO: host 2 value 20 weak null
// WEAK: host 1 value 10 weak 7

#include <stdio.h>
#include <windows.h>

#define API __attribute__((visibility("default")))

#if defined(HOST)
API int host_value;
API int host_id(void) { return HOST; }

int main(int argc, char **argv) {
  host_value = HOST * 10;
  if (argc > 2 && !LoadLibraryA(argv[2]))
    return 2;
  HMODULE plugin = LoadLibraryA(argv[1]);
  if (!plugin)
    return 3;
  int (*report)(void) = (int (*)(void))GetProcAddress(plugin, "report");
  return report ? report() : 4;
}
#elif defined(WEAKDEP)
API int weak_fn(void) { return 7; }
#elif defined(PLUGIN)
API extern int host_value;
API int host_id(void);
API __attribute__((weak)) int weak_fn(void);

API int report(void) {
  if (weak_fn)
    printf("host %d value %d weak %d\n", host_id(), host_value, weak_fn());
  else
    printf("host %d value %d weak null\n", host_id(), host_value);
  fflush(stdout);
  return 0;
}
#endif
