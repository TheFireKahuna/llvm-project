// DEFINE: %{cc} = %clang -fno-exceptions
// RUN: %{cc} -shared -DCOUNT %s -o %t-cnt.dll
// RUN: %{cc} -shared -DSIBLING=1 %s %t-cnt.dll.lib -o %t-d1.dll
// RUN: %{cc} -shared -DSIBLING=2 %s %t-cnt.dll.lib -o %t-d2.dll
// RUN: %{cc} -shared -DSIBLING=3 %s %t-cnt.dll.lib -o %t-d3.dll
// RUN: %{cc} %s %t-d1.dll.lib %t-d2.dll.lib %t-cnt.dll.lib -o %t.exe
// RUN: %{cc} -DOWN %s %t-d1.dll.lib %t-d2.dll.lib %t-cnt.dll.lib \
// RUN:   -Wl,-ignore-offers:%basename_t.tmp-d1.dll \
// RUN:   -Wl,-ignore-offers:%basename_t.tmp-d2.dll -o %t-own.exe
// RUN: %run %t.exe %t-d3.dll | FileCheck %s
// RUN: %run %t-own.exe %t-d3.dll | FileCheck --check-prefix=OWN %s

// REQUIRES: target={{x86_64-.*}}

// True sibling DLLs, which neither links against the other, each keep an
// inline thread_local and a template's static thread_local, and so does a
// plugin that the process loads once it has started: each thread has one
// instance of each in the program, constructed once, which every image
// reaches, whether the executable keeps one or none.

// CHECK: shared: same 1, distinct 1, values 1, constructed 3 3
// OWN:   own: same 1, distinct 1, values 1, constructed 3 3

#include <windows.h>
#include <stdio.h>

#define EXPORT extern "C" __declspec(dllexport)
#define CAT(A, B) CAT2(A, B)
#define CAT2(A, B) A##B

#ifdef COUNT
static volatile LONG Count[3];
EXPORT int construct(int N) { return InterlockedIncrement(&Count[N]); }
EXPORT int constructed(int N) { return Count[N]; }
#else
extern "C" __declspec(dllimport) int construct(int N);
extern "C" __declspec(dllimport) int constructed(int N);

#if defined(SIBLING) || defined(OWN)
template <int N> struct C {
  int V = 1000 * N + construct(N);
};
inline thread_local C<1> IV;
template <class T> struct X {
  static thread_local T M;
};
template <class T> thread_local T X<T>::M;
#endif

#ifdef SIBLING
// Each sibling's view of the thread's instances.
EXPORT void CAT(addrs, SIBLING)(const void **A) {
  A[0] = &IV;
  A[1] = &X<C<2>>::M;
}
#else
typedef void Addrs(const void **);
extern "C" __declspec(dllimport) void addrs1(const void **);
extern "C" __declspec(dllimport) void addrs2(const void **);

enum { Threads = 3, Views = 4 };
static Addrs *Plugin;
static const void *Seen[Threads][2];
static volatile LONG Same = 1, Values = 1;

static DWORD WINAPI run(void *Arg) {
  int T = (int)(INT_PTR)Arg;
  const void *A[Views][2] = {};
  // The siblings construct in turn, the plugin last.
  Addrs *Order[] = {T & 1 ? addrs2 : addrs1, T & 1 ? addrs1 : addrs2, Plugin};
  for (int I = 0; I != 3; ++I)
    Order[I](A[I]);
#ifdef OWN
  A[3][0] = &IV;
  A[3][1] = &X<C<2>>::M;
#else
  A[3][0] = A[0][0];
  A[3][1] = A[0][1];
#endif
  for (int I = 1; I != Views; ++I)
    if (A[I][0] != A[0][0] || A[I][1] != A[0][1])
      Same = 0;
  // Each instance's value, which its construction gives.
  if (*(const int *)A[0][0] / 1000 != 1 || *(const int *)A[0][1] / 1000 != 2)
    Values = 0;
  Seen[T][0] = A[0][0];
  Seen[T][1] = A[0][1];
  return 0;
}

int main(int, char **Argv) {
  HMODULE M = LoadLibraryA(Argv[1]);
  Plugin = M ? (Addrs *)GetProcAddress(M, "addrs3") : 0;
  if (!Plugin)
    return 1;
  HANDLE H[Threads];
  for (int T = 1; T != Threads; ++T)
    H[T] = CreateThread(0, 0, run, (void *)(INT_PTR)T, 0, 0);
  run(0);
  WaitForMultipleObjects(Threads - 1, H + 1, TRUE, INFINITE);
  int Distinct = 1;
  for (int T = 0; T != Threads; ++T)
    for (int U = 0; U != T; ++U)
      Distinct &= Seen[T][0] != Seen[U][0] && Seen[T][1] != Seen[U][1];
#ifdef OWN
  const char *Name = "own";
#else
  const char *Name = "shared";
#endif
  printf("%s: same %d, distinct %d, values %d, constructed %d %d\n", Name,
         (int)Same, Distinct, (int)Values, constructed(1), constructed(2));
  return 0;
}
#endif
#endif
