// RUN: rm -rf %t && mkdir %t
// RUN: %clang_wincrt -std=c++17 -fno-exceptions -shared -DDLL_A %s -o %t/a.dll
// RUN: %clang_wincrt -std=c++17 -fno-exceptions -shared -DDLL_B %s -o %t/b.dll
// RUN: %clang_wincrt -std=c++17 -fno-exceptions -shared -DDLL_C %s %t/b.dll.lib \
// RUN:   -o %t/c.dll
// RUN: %clang_wincrt -std=c++17 -fno-exceptions -shared -DDLL_P %s -o %t/p.dll
// RUN: %clang_wincrt -std=c++17 -fno-exceptions -shared -DDLL_D %s -o %t/d.dll
// RUN: %clang_wincrt -std=c++17 -fno-exceptions -shared -DDLL_Q %s %t/d.dll.lib \
// RUN:   -Wl,-delayload:d.dll -o %t/q.dll
// RUN: %clang_wincrt -std=c++17 -fno-exceptions -DHOST %s %t/a.dll.lib \
// RUN:   %t/b.dll.lib %t/c.dll.lib -o %t/host.exe
// RUN: %clang_wincrt -std=c++17 -fno-exceptions -DPLUGIN_HOST %s \
// RUN:   -o %t/plugin_host.exe
// RUN: %clang_wincrt -std=c++17 -fno-exceptions -DDELAY_HOST %s \
// RUN:   %t/d.dll.lib %t/p.dll.lib -Wl,-delayload:d.dll -Wl,-delayload:p.dll \
// RUN:   -o %t/delay_host.exe
// RUN: %run %t/host.exe %t/p.dll | FileCheck --check-prefix=HOST %s
// RUN: %run %t/plugin_host.exe %t/q.dll | FileCheck --check-prefix=PLUGIN %s
// RUN: %run %t/delay_host.exe | FileCheck --check-prefix=DELAY %s

// An inline variable has one instance in the program, as on ELF. a.dll and
// b.dll, true siblings, each keep it preemptible, and the executable's link
// picks a.dll's; c.dll imports it from b.dll and reaches the same one. A
// plugin that keeps it binds to that instance when loaded, and stays loaded
// after FreeLibrary, since others use the instance it may have initialized. A
// DLL that a plugin delay-loads joins the plugin's scope and binds to the
// plugin's instance. Of two delay-loaded DLLs that keep it, the first to load
// keeps its instance and the second binds to it.

// HOST: same 1 value 3
// HOST: plugin same 1 value 4 pinned 1
// PLUGIN: same 1 value 2
// DELAY: same 1 value 2

#include <stdio.h>
#include <windows.h>

#define API __attribute__((visibility("default")))

struct API Counter {
  static inline int value = 0;
};

#define BUMP(name)                                                             \
  API int *name() {                                                            \
    ++Counter::value;                                                          \
    return &Counter::value;                                                    \
  }

#if defined(DLL_A)
BUMP(bumpA)
#elif defined(DLL_B)
BUMP(bumpB)
#elif defined(DLL_C)
BUMP(bumpC)
#elif defined(DLL_P)
BUMP(bumpP)
#elif defined(DLL_D)
BUMP(bumpD)
#elif defined(DLL_Q)
API int *bumpD();
BUMP(bumpQ)
API int *viaD() { return bumpD(); }
#elif defined(HOST)
API int *bumpA();
API int *bumpB();
API int *bumpC();

int main(int argc, char **argv) {
  int *a = bumpA(), *b = bumpB(), *c = bumpC();
  printf("same %d value %d\n", a == b && b == c, *a);
  HMODULE plugin = LoadLibraryA(argv[1]);
  if (!plugin)
    return 2;
  int *(*bumpP)() = (int *(*)())GetProcAddress(plugin, "_Z5bumpPv");
  if (!bumpP)
    return 3;
  int *p = bumpP();
  FreeLibrary(plugin);
  printf("plugin same %d value %d pinned %d\n", p == a, *a,
         GetModuleHandleA("p.dll") != NULL);
  return 0;
}
#elif defined(DELAY_HOST)
API int *bumpD();
API int *bumpP();

int main() {
  int *d = bumpD(), *p = bumpP();
  printf("same %d value %d\n", d == p, *d);
  return 0;
}
#elif defined(PLUGIN_HOST)
int main(int argc, char **argv) {
  HMODULE plugin = LoadLibraryA(argv[1]);
  if (!plugin)
    return 2;
  int *(*bumpQ)() = (int *(*)())GetProcAddress(plugin, "_Z5bumpQv");
  int *(*viaD)() = (int *(*)())GetProcAddress(plugin, "_Z4viaDv");
  if (!bumpQ || !viaD)
    return 3;
  int *q = bumpQ(), *d = viaD();
  printf("same %d value %d\n", q == d, *q);
  return 0;
}
#endif
