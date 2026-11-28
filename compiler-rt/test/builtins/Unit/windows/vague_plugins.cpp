// DEFINE: %{cc} = %clang -fno-exceptions -fno-threadsafe-statics
// RUN: %{cc} -shared -DCOUNT %s -o %t-cnt.dll
// RUN: %{cc} -shared -DPLUGIN=1 %s %t-cnt.dll.lib -o %t-p1.dll
// RUN: %{cc} -shared -DPLUGIN=2 %s %t-cnt.dll.lib -o %t-p2.dll
// RUN: %{cc} -shared -DWIDE %s -o %t-wide.dll
// RUN: %{cc} -DHOST %s %t-cnt.dll.lib -o %t-host.exe
// RUN: %{cc} %s %t-cnt.dll.lib -o %t.exe
// RUN: %{cc} -DINITIAL %s %t-p1.dll.lib %t-cnt.dll.lib -o %t-initial.exe
// RUN: %{cc} -DINITIAL -DDELAY %s %t-p1.dll.lib %t-p2.dll.lib %t-cnt.dll.lib \
// RUN:   -Wl,-delayload:%basename_t.tmp-p2.dll -o %t-delay.exe
// RUN: %{cc} -shared -DEARLY %s -o %t-early.dll
// RUN: %{cc} -shared -DEARLY -DTLS_CALLBACK %s -o %t-callback.dll
// RUN: %{cc} -DINITIAL -DEARLY %s %t-p1.dll.lib %t-early.dll.lib \
// RUN:   %t-cnt.dll.lib -o %t-early.exe
// RUN: %{cc} -DINITIAL -DEARLY %s %t-p1.dll.lib %t-callback.dll.lib \
// RUN:   %t-cnt.dll.lib -o %t-callback.exe
// RUN: %clang -fno-exceptions --target=x86_64-pc-windows-msvc -fuse-ld=lld \
// RUN:   -nostdlib -Wl,-entry:start,-subsystem:console -DFOREIGN %s \
// RUN:   %t-p1.dll.lib %t-p2.dll.lib %t-cnt.dll.lib -o %t-foreign.exe
// RUN: %run %t-host.exe %t-p1.dll | FileCheck --check-prefix=HOST %s
// RUN: %run %t.exe %t-p1.dll %t-p2.dll %t-wide.dll | FileCheck %s
// RUN: %run %t-initial.exe %t-p2.dll | FileCheck --check-prefix=INITIAL %s
// RUN: %run %t-delay.exe | FileCheck --check-prefix=INITIAL %s
// RUN: %run %t-early.exe | FileCheck --check-prefix=EARLY %s
// RUN: %run %t-callback.exe | FileCheck --check-prefix=EARLY %s
// RUN: %run %t-foreign.exe | FileCheck --check-prefix=FOREIGN %s
// RUN: %{cc} -shared -DEMPTY %s %t-cnt.dll.lib -o %t-r.dll
// RUN: %{cc} -DINITIAL -DDELAY %s %t-p1.dll.lib %t-r.dll.lib %t-cnt.dll.lib \
// RUN:   -o %t-replaced.exe
// RUN: %{cc} -shared -DPLUGIN=2 %s %t-cnt.dll.lib -o %t-r.dll
// RUN: %run %t-replaced.exe | FileCheck --check-prefix=INITIAL %s
// RUN: %{cc} -mguard=cf -shared -DOWNER %s -Xlinker -guard:cf -o %t-owner.dll
// RUN: %{cc} -mguard=cf -shared -DCALLER %s -Xlinker -guard:cf \
// RUN:   -o %t-caller.dll
// RUN: %{cc} -mguard=cf -DGUARD %s %t-owner.dll.lib \
// RUN:   -Xlinker -guard:cf,exportsuppress -o %t-guard.exe
// RUN: %run %t-guard.exe %t-caller.dll | FileCheck --check-prefix=GUARD %s

// REQUIRES: target={{x86_64-.*}}

// A DLL that the process loads after it started binds each vague entity that
// it keeps preemptible to the instance that the process already has: the
// executable's, one that a DLL that the process loaded as it started keeps, or
// one that a DLL loaded before it keeps, as long as the two entries state one
// entity, its size, alignment and init kind. An inline variable, a function's
// static with its guard, and an inline function's address are then one in
// every image, and each initializer runs once. A DLL that keeps another
// version of an entity, or one that it keeps hidden, keeps its own. A DLL
// whose instance another binds stays loaded; one that binds only to the
// executable's unloads, and binds again when it loads again. So does a DLL
// that another loads while the process starts, from its DllMain or its TLS
// callback, and one that the process loads as it starts but the executable's
// link did not see, as an executable of another producer's links see none,
// or saw as a file that kept nothing, which another has replaced.
// A function that a DLL exports and never takes the address of, which control
// flow guard admits a call of only once the loader grants it, is one that a
// DLL binding to it can call.

// HOST:      host: iv 1, local 1, function 1, inits 2
// HOST-NEXT: host: unloaded 1
// HOST-NEXT: host: iv 1, local 1, function 1, inits 2

// CHECK:      plugins: iv 1, local 1, function 1, inits 2
// CHECK-NEXT: plugins: first loaded 1
// CHECK-NEXT: shapes: same 1, size 0, alignment 0, hidden 0, works 1

// INITIAL: initial: iv 1, local 1, function 1, inits 2

// EARLY: early: iv 1, local 1, function 1, inits 2

// FOREIGN: foreign: iv 1, local 1, function 1, inits 2

// GUARD: guard: same 1, call 42

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define EXPORT extern "C" __declspec(dllexport)

#ifdef COUNT
static int Count;
EXPORT int init(void) { return ++Count; }
EXPORT int inits(void) { return Count; }
EXPORT void compare(const char *Name, const void **A, const void **B) {
  printf("%s: iv %d, local %d, function %d, inits %d\n", Name, A[0] == B[0],
         A[1] == B[1], A[2] == B[2], Count);
  fflush(stdout);
}
#elif defined(FOREIGN)
// An executable that another producer's link made, which keeps nothing and
// loads both plugins as it starts.
extern "C" __declspec(dllimport) void addrs1(const void **);
extern "C" __declspec(dllimport) void addrs2(const void **);
extern "C" __declspec(dllimport) void compare(const char *, const void **,
                                              const void **);
extern "C" int start() {
  const void *A[3], *B[3];
  addrs1(A);
  addrs2(B);
  compare("foreign", A, B);
  return 0;
}
#elif defined(EMPTY)
// The second plugin as the executable's link saw it, which keeps nothing.
extern "C" __declspec(dllimport) int inits(void);
EXPORT void addrs2(const void **A) { A[0] = A[1] = A[2] = (void *)inits; }
#elif defined(OWNER) || defined(CALLER) || defined(GUARD)
template <int I> int add(int X) { return X + 41 + I; }
#  ifdef OWNER
// The DLL that the process loads as it starts instantiates add<0> and exports
// it, and never takes its address.
template __attribute__((visibility("default"))) int add<0>(int);
EXPORT int owned(int X) { return add<0>(X); }
#  elif defined(CALLER)
EXPORT void *taken(void) { return (void *)&add<0>; }
EXPORT int called(int X) {
  int (*volatile F)(int) = &add<0>;
  return F(X);
}
#  else
// An executable that turns export suppression on loads a DLL that calls
// through its own cell, which binds to the owner's add<0>.
extern "C" __declspec(dllimport) int owned(int);
int main(int, char **Argv) {
  HMODULE M = LoadLibraryA(Argv[1]), Owner;
  int Call = ((int (*)(int))GetProcAddress(M, "called"))(1);
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     (LPCSTR)&owned, &Owner);
  printf("guard: same %d, call %d\n",
         ((void *(*)(void))GetProcAddress(M, "taken"))() ==
             (void *)GetProcAddress(Owner, "_Z3addILi0EEii"),
         Call);
}
#  endif
#elif defined(EARLY) && !defined(INITIAL)
// Loads the second plugin, whose file is named as this DLL's is with p2 for
// its last part, as the process starts.
typedef void Addrs(const void **);
static HMODULE Plugin;
static void load(HMODULE Self) {
  char Path[MAX_PATH];
  GetModuleFileNameA(Self, Path, MAX_PATH);
  strcpy(strrchr(Path, '-') + 1, "p2.dll");
  Plugin = LoadLibraryA(Path);
}
EXPORT void early(const void **A) {
  ((Addrs *)GetProcAddress(Plugin, "addrs2"))(A);
}
#ifdef TLS_CALLBACK
#pragma comment(linker, "/include:_tls_used")
static void NTAPI callback(PVOID Module, DWORD Reason, PVOID) {
  if (Reason == DLL_PROCESS_ATTACH)
    load((HMODULE)Module);
}
__attribute__((used, section(".CRT$XLB"))) static const PIMAGE_TLS_CALLBACK
    Callback = callback;
#else
BOOL WINAPI DllMain(HINSTANCE Module, DWORD Reason, LPVOID) {
  if (Reason == DLL_PROCESS_ATTACH)
    load(Module);
  return TRUE;
}
#endif
#else

#ifdef WIDE
inline int same = 1;
inline long long size = 1;
alignas(64) inline int alignment = 1;
__attribute__((visibility("hidden"))) inline int hidden = 1;
EXPORT void wide(const void **A) {
  const void *V[] = {&same, &size, &alignment, &hidden};
  for (int I = 0; I != 4; ++I)
    A[I] = V[I];
}
EXPORT int wideWorks(void) {
  size += 1LL << 40;
  return (int)(size >> 40) + alignment + hidden;
}
#else
extern "C" __declspec(dllimport) int init(void);
extern "C" __declspec(dllimport) int inits(void);
#endif

#if defined(PLUGIN) || defined(HOST)
inline int iv = init();
inline int &local() {
  static int S = init();
  return S;
}
inline void function() {}
inline void fill(const void **A) {
  A[0] = &iv;
  A[1] = &local();
  A[2] = (const void *)&function;
}
#endif

#ifdef PLUGIN
inline int same = 1;
inline int size = 1;
inline int alignment = 1;
inline int hidden = 1;
#define CAT2(A, B) A##B
#define CAT(A, B) CAT2(A, B)
EXPORT void CAT(addrs, PLUGIN)(const void **A) { fill(A); }
EXPORT void shapes(const void **A) {
  const void *V[] = {&same, &size, &alignment, &hidden};
  for (int I = 0; I != 4; ++I)
    A[I] = V[I];
}
#elif !defined(WIDE)
typedef void Addrs(const void **);
extern "C" __declspec(dllimport) void compare(const char *, const void **,
                                              const void **);

#if defined(HOST)
// The executable keeps every entity and initializes them before the plugin
// loads, once and then again.
int main(int, char **Argv) {
  const void *A[3], *B[3];
  fill(A);
  for (int Round = 0; Round != 2; ++Round) {
    HMODULE M = LoadLibraryA(Argv[1]);
    ((Addrs *)GetProcAddress(M, "addrs1"))(B);
    compare("host", A, B);
    FreeLibrary(M);
    if (!Round)
      printf("host: unloaded %d\n", !GetModuleHandleA(Argv[1]));
  }
}
#elif defined(INITIAL)
extern "C" __declspec(dllimport) void addrs1(const void **);
#ifdef EARLY
// The executable keeps none of them; the DLL that it loads as it starts keeps
// them, and the one that another DLL loads as the process starts binds to it.
extern "C" __declspec(dllimport) void early(const void **);
int main() {
  const void *A[3], *B[3];
  addrs1(A);
  early(B);
  compare("early", A, B);
}
#elif defined(DELAY)
// The executable keeps none of them; the DLL that it loads as it starts keeps
// them, and the one that it delay-loads binds to it at its first call.
extern "C" __declspec(dllimport) void addrs2(const void **);
int main() {
  const void *A[3], *B[3];
  addrs1(A);
  addrs2(B);
  compare("initial", A, B);
}
#else
int main(int, char **Argv) {
  const void *A[3], *B[3];
  addrs1(A);
  ((Addrs *)GetProcAddress(LoadLibraryA(Argv[1]), "addrs2"))(B);
  compare("initial", A, B);
}
#endif
#else
// The executable keeps none of them: the second plugin binds to the first,
// which then stays loaded, and the wide one binds only what it shares.
int main(int, char **Argv) {
  const void *A[4], *B[4];
  HMODULE P1 = LoadLibraryA(Argv[1]), P2 = LoadLibraryA(Argv[2]);
  ((Addrs *)GetProcAddress(P1, "addrs1"))(A);
  ((Addrs *)GetProcAddress(P2, "addrs2"))(B);
  compare("plugins", A, B);
  FreeLibrary(P1);
  printf("plugins: first loaded %d\n", GetModuleHandleA(Argv[1]) != nullptr);
  HMODULE W = LoadLibraryA(Argv[3]);
  ((Addrs *)GetProcAddress(P2, "shapes"))(A);
  ((Addrs *)GetProcAddress(W, "wide"))(B);
  int Works = ((int (*)(void))GetProcAddress(W, "wideWorks"))() == 3 &&
              *(const int *)A[1] == 1 && *(const int *)A[2] == 1;
  printf("shapes: same %d, size %d, alignment %d, hidden %d, works %d\n",
         A[0] == B[0], A[1] == B[1], A[2] == B[2], A[3] == B[3], Works);
}
#endif
#endif
#endif
