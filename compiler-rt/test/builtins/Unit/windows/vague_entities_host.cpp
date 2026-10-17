// DEFINE: %{cxx} = %clang_wincrt -std=c++17 -fno-exceptions
// RUN: rm -rf %t && mkdir %t
// RUN: %{cxx} -DHOST %s -o %t/host.exe -Wl,-implib:%t/host.lib
// RUN: %{cxx} -shared -DPLUGIN %s %t/host.lib -o %t/linked.dll
// RUN: %{cxx} -shared -DPLUGIN %s -o %t/unlinked.dll
// RUN: %run %t/host.exe %t/linked.dll | FileCheck %s
// RUN: %run %t/host.exe %t/unlinked.dll | FileCheck %s

// An executable's exports are the instances it shares, as an ELF
// executable's dynamic symbols are. A plugin linked against the executable's
// import library takes its offer and imports the instance from it; one linked
// without it finds the instance by name when it loads. Either way the plugin
// and the executable share one instance of an inline variable.

// CHECK: same 1 value 2

#include <stdio.h>
#include <windows.h>

#define API __attribute__((visibility("default")))

struct API Shared {
  static inline int value = 0;
};

#if defined(HOST)
int main(int argc, char **argv) {
  ++Shared::value;
  HMODULE Plugin = LoadLibraryA(argv[1]);
  if (!Plugin)
    return 2;
  int *(*Bump)() = (int *(*)())GetProcAddress(Plugin, "_Z4bumpv");
  if (!Bump)
    return 3;
  int *P = Bump();
  printf("same %d value %d\n", P == &Shared::value, Shared::value);
  return 0;
}
#elif defined(PLUGIN)
API int *bump() {
  ++Shared::value;
  return &Shared::value;
}
#endif
