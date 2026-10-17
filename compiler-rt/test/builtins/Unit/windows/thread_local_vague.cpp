// DEFINE: %{cxx} = %clang_wincrt -std=c++17 -fno-exceptions
// RUN: rm -rf %t && mkdir %t
// RUN: %{cxx} -shared -DDLL_A %s -o %t/a.dll
// RUN: %{cxx} -shared -DDLL_B %s -o %t/b.dll
// RUN: %{cxx} -shared -DDLL_C %s -o %t/c.dll
// RUN: %{cxx} -shared -DPLUGIN %s -o %t/plugin.dll
// RUN: %{cxx} -DHOST %s %t/a.dll.lib %t/b.dll.lib %t/c.dll.lib \
// RUN:   -o %t/host.exe
// RUN: %run %t/host.exe %t/plugin.dll | FileCheck %s
// RUN: mkdir %t/lto
// RUN: %{cxx} -flto -shared -DDLL_A %s -o %t/lto/a.dll
// RUN: %{cxx} -flto -shared -DDLL_B %s -o %t/lto/b.dll
// RUN: %{cxx} -flto -shared -DDLL_C %s -o %t/lto/c.dll
// RUN: %{cxx} -flto -shared -DPLUGIN %s -o %t/lto/plugin.dll
// RUN: %{cxx} -DHOST %s %t/lto/a.dll.lib %t/lto/b.dll.lib %t/lto/c.dll.lib \
// RUN:   -o %t/lto/host.exe
// RUN: %run %t/lto/host.exe %t/lto/plugin.dll | FileCheck %s

// A vague thread-local variable has one instance on each thread, as on ELF:
// the executable takes a.dll's offer of each, along with its guard; b.dll
// and c.dll, true siblings, each keep one preemptible, and the executable's
// link picks b.dll's, which a plugin binds to when it loads. Each is
// initialized once on each thread, by whichever image's code reaches it first
// there, and each thread has instances of its own, also where the DLLs are
// built with LTO.

// CHECK: main: offered 1 inits 1 fixed 1 local 1 member 1
// CHECK: main: siblings 1
// CHECK: main: plugin 1
// CHECK: thread: offered 1 inits 1 fixed 1 local 1 member 1 own 1
// CHECK: thread: siblings 1
// CHECK: thread: plugin 1

#include <stdio.h>
#include <windows.h>

#define API __attribute__((visibility("default")))

#if !defined(DLL_A)
struct API Sibling {
  static inline thread_local int value = 1;
};
#endif

#if defined(DLL_A) || defined(HOST)
API int countInit();

struct API Shared {
  static inline thread_local int dynamic = countInit();
  static inline thread_local int fixed = 5;
};
template <class T> struct Member {
  static thread_local int value;
};
template <class T> thread_local int Member<T>::value = countInit();
API inline int &local() {
  static thread_local int value = countInit();
  return value;
}

struct Addresses {
  int *dynamic, *fixed, *local, *member;
};

#define ADDRESSES(name)                                                        \
  API Addresses name() {                                                       \
    return {&Shared::dynamic, &Shared::fixed, &local(), &Member<int>::value};  \
  }
#endif

#if defined(DLL_A)
static thread_local int Inits;
API int countInit() { return ++Inits; }
API int inits() { return Inits; }
ADDRESSES(addressesA)
#elif defined(DLL_B)
API int *siblingB() { return &Sibling::value; }
#elif defined(DLL_C)
API int *siblingC() { return &Sibling::value; }
#elif defined(PLUGIN)
API int *siblingP() { return &Sibling::value; }
#elif defined(HOST)
API int inits();
API Addresses addressesA();
API int *siblingB();
API int *siblingC();
ADDRESSES(addressesHost)

static int *(*SiblingP)();

static void report(const char *who, Addresses *own) {
  Addresses a = addressesA(), h = addressesHost();
  printf("%s: offered %d inits %d fixed %d local %d member %d", who,
         a.dynamic == h.dynamic, inits() == 3, a.fixed == h.fixed,
         a.local == h.local, a.member == h.member);
  if (own)
    printf(" own %d", own->dynamic != h.dynamic && own->fixed != h.fixed &&
                          own->local != h.local && own->member != h.member);
  printf("\n%s: siblings %d\n", who,
         siblingB() == siblingC() && siblingB() == &Sibling::value);
  printf("%s: plugin %d\n", who, SiblingP() == siblingB());
}

static DWORD WINAPI thread(void *main) {
  report("thread", (Addresses *)main);
  return 0;
}

int main(int argc, char **argv) {
  HMODULE plugin = LoadLibraryA(argv[1]);
  if (!plugin)
    return 2;
  SiblingP = (int *(*)())GetProcAddress(plugin, "_Z8siblingPv");
  if (!SiblingP)
    return 3;
  report("main", nullptr);
  Addresses main = addressesHost();
  HANDLE t = CreateThread(nullptr, 0, thread, &main, 0, nullptr);
  WaitForSingleObject(t, INFINITE);
  return 0;
}
#endif
