// DEFINE: %{cxx} = %clang_wincrt -std=c++17 -fno-exceptions \
// DEFINE:   -fno-threadsafe-statics
// RUN: rm -rf %t && mkdir %t
// RUN: %{cxx} -shared -DDLL_A %s -o %t/a.dll
// RUN: %{cxx} -shared -DDLL_B %s -o %t/b.dll
// RUN: %{cxx} -shared -DDLL_C %s %t/b.dll.lib -o %t/c.dll
// RUN: %{cxx} -shared -DDLL_P %s -o %t/p.dll
// RUN: %{cxx} -shared -DDLL_K %s -o %t/k.dll
// RUN: %{cxx} -shared -DDLL_D %s -o %t/d.dll
// RUN: %{cxx} -shared -DDLL_Q %s %t/d.dll.lib \
// RUN:   -Wl,-delayload:d.dll -o %t/q.dll
// RUN: %{cxx} -DHOST %s %t/a.dll.lib %t/b.dll.lib %t/c.dll.lib \
// RUN:   -o %t/host.exe
// RUN: %{cxx} -DPLUGIN_HOST %s -o %t/plugin_host.exe
// RUN: %{cxx} -DDELAY_HOST %s %t/d.dll.lib %t/p.dll.lib \
// RUN:   -Wl,-delayload:d.dll -Wl,-delayload:p.dll -o %t/delay_host.exe
// RUN: %run %t/host.exe %t/p.dll %t/k.dll | FileCheck --check-prefix=HOST %s
// RUN: %run %t/plugin_host.exe %t/q.dll %t/p.dll | \
// RUN:   FileCheck --check-prefix=PLUGIN %s
// RUN: %run %t/delay_host.exe | FileCheck --check-prefix=DELAY %s

// An inline variable has one instance in the program, as on ELF. a.dll and
// b.dll, true siblings, each keep it preemptible, and the executable's link
// picks a.dll's; c.dll imports it from b.dll and reaches the same one. A
// plugin that keeps it binds to that instance when loaded, and FreeLibrary
// unloads it, since nothing it registered refers to that instance. A plugin
// whose code initializes a.dll's instance of a static local, and so registers
// its destructor, stays loaded, since unloading it would destroy an object
// a.dll uses. An immutable inline variable has one address too. A
// DLL that a plugin delay-loads joins the plugin's scope and binds to the
// plugin's instance, which a plugin loaded later does not see, as under
// RTLD_LOCAL. Of two delay-loaded DLLs that keep it, the first to load
// keeps its instance and the second binds to it.

// HOST: same 1 value 3
// HOST: fixed same 1
// HOST: plugin fixed same 1
// HOST: constant same 1 value 4 pinned 0
// HOST: guarded same 1 value 7 pinned 1
// PLUGIN: same 1 value 2
// PLUGIN: other same 0 value 1
// DELAY: same 1 value 2

#include <stdio.h>
#include <windows.h>

#define API __attribute__((visibility("default")))

struct API Counter {
  static inline int value = 0;
};

// Immutable, which every image may read from its own copy, but whose address
// is the program's one.
struct API Fixed {
  static inline const int value = 5;
};

// Initialized on the first call, by whichever image's code makes it; the
// test links no C++ runtime, so the guard is tested inline.
struct API Tracker {
  int x;
  Tracker() : x(7) {}
  ~Tracker() { x = -1; }
};
API inline Tracker &tracker() {
  static Tracker t;
  return t;
}

#define BUMP(name)                                                             \
  API int *name() {                                                            \
    ++Counter::value;                                                          \
    return &Counter::value;                                                    \
  }

#if defined(DLL_A)
BUMP(bumpA)
API const int *fixedA() { return &Fixed::value; }
API int *viaA() { return &tracker().x; }
#elif defined(DLL_B)
BUMP(bumpB)
API const int *fixedB() { return &Fixed::value; }
#elif defined(DLL_C)
BUMP(bumpC)
#elif defined(DLL_P)
BUMP(bumpP)
API int *viaP() { return &tracker().x; }
#elif defined(DLL_K)
BUMP(bumpK)
API const int *fixedK() { return &Fixed::value; }
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
API int *viaA();
API const int *fixedA();
API const int *fixedB();

int main(int argc, char **argv) {
  int *a = bumpA(), *b = bumpB(), *c = bumpC();
  printf("same %d value %d\n", a == b && b == c, *a);
  printf("fixed same %d\n", fixedA() == fixedB() && fixedA() == &Fixed::value);

  HMODULE constant = LoadLibraryA(argv[2]);
  if (!constant)
    return 2;
  int *(*bumpK)() = (int *(*)())GetProcAddress(constant, "_Z5bumpKv");
  if (!bumpK)
    return 3;
  int *k = bumpK();
  const int *(*fixedK)() =
      (const int *(*)())GetProcAddress(constant, "_Z6fixedKv");
  if (!fixedK)
    return 3;
  printf("plugin fixed same %d\n", fixedK() == fixedA());
  FreeLibrary(constant);
  printf("constant same %d value %d pinned %d\n", k == a, *a,
         GetModuleHandleA("k.dll") != NULL);

  HMODULE guarded = LoadLibraryA(argv[1]);
  if (!guarded)
    return 4;
  int *(*viaP)() = (int *(*)())GetProcAddress(guarded, "_Z4viaPv");
  if (!viaP)
    return 5;
  int *t = viaP();
  FreeLibrary(guarded);
  printf("guarded same %d value %d pinned %d\n", t == viaA(), *t,
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
  HMODULE other = LoadLibraryA(argv[2]);
  if (!other)
    return 4;
  int *(*bumpP)() = (int *(*)())GetProcAddress(other, "_Z5bumpPv");
  if (!bumpP)
    return 5;
  int *p = bumpP();
  printf("other same %d value %d\n", p == d, *p);
  return 0;
}
#endif
