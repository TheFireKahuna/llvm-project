// DEFINE: %{cc} = %clang -fno-exceptions -std=c++20
// RUN: %{cc} -shared -DOTHER %s -o %t-other.dll
// RUN: %{cc} -shared -DRT %s -o %t-rt.dll
// RUN: %{cc} -shared -DDEP %s -o %t-dep.dll
// RUN: %clang -c -x c -DDLLC %s -o %t-c.o
// RUN: %{cc} -shared -DDLL %s %t-c.o %t-other.dll.lib %t-rt.dll.lib \
// RUN:   %t-dep.dll.lib -o %t.dll
// RUN: %{cc} -shared -DPLAIN %s %t-other.dll.lib -o %t-plain.dll
// RUN: %{cc} -shared -DSOLO %s %t-other.dll.lib -o %t-solo.dll
// RUN: %{cc} -shared -DSTART %s %t-other.dll.lib %t.dll.lib -o %t-start.dll
// RUN: %{cc} -DEXE %s %t-rt.dll.lib -o %t.exe
// RUN: %clang -DSTATIC -x c %s -x none %t-other.dll.lib %t-start.dll.lib %t.dll.lib \
// RUN:   -o %t-static.exe
// RUN: llvm-readobj --file-headers --coff-imports %t-plain.dll \
// RUN:   | FileCheck --check-prefix=PLAIN %s
// RUN: %run %t.exe %t.dll %t-solo.dll | FileCheck %s
// RUN: %run %t-static.exe | FileCheck --check-prefix=STATIC %s

// REQUIRES: target={{x86_64-.*}}

// A thread-local variable whose constant initializer holds an address inside
// another DLL's object, which no name the DLL exports is, holds what the
// image's fill writes in every thread, in each form the languages have: a
// thread that copied the template before the fill wrote it has its copy
// written too, where it still holds the value that no pointer holds, so that
// a thread's own store stands. For a DLL that the process loads after it
// starts, that is the loading thread, every thread already running, one that
// a DLL the load initializes starts, and one started later; for an
// executable, the main thread and one that a DLL starts as the process
// starts. A replaced overridable default held in a thread-local is the
// replacement in every thread. A thread that the load cannot reach fails the
// load, and a thread that impersonates loads the DLL and impersonates still.
// A DLL with no such word writes no other thread.

// CHECK:      loading: right 9, own store 1
// CHECK-NEXT: running: right 9
// CHECK-NEXT: running: right 9
// CHECK-NEXT: during: right 9
// CHECK-NEXT: after: right 9
// CHECK-NEXT: unreachable: 1114
// CHECK-NEXT: impersonating: loaded 1, still 1, right 1

// STATIC:      main: right 9, exe 1, own store 1
// STATIC-NEXT: started: right 9, exe 1

// PLAIN:     AddressOfEntryPoint:
// PLAIN-NOT: NtSuspendThread
// PLAIN-NOT: NtQuerySystemInformation

#include <windows.h>

typedef __SIZE_TYPE__ size_t;
#define NVALUES 9

#if defined(OTHER)
__declspec(dllexport) int table[8];
#elif defined(RT)
[[gnu::weak, clang::overridable_default(0)]] __declspec(dllexport) void *
operator new(size_t n) {
  return HeapAlloc(GetProcessHeap(), 0, n ? n : 1);
}
#elif defined(DEP)
// A dependency of the DLL, whose attach where the process loads them later
// starts a thread and waits until it has copied the DLL's template.
static HANDLE go, thread;
static void (*values)(const void **);
static const void *during[NVALUES];
extern "C" __declspec(dllexport) const void **
dep_during(void (*read)(const void **)) {
  values = read;
  SetEvent(go);
  WaitForSingleObject(thread, INFINITE);
  return during;
}
static DWORD WINAPI start(void *) {
  WaitForSingleObject(go, INFINITE);
  values(during);
  return 0;
}
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, void *reserved) {
  if (reason != DLL_PROCESS_ATTACH || reserved)
    return TRUE;
  go = CreateEventA(0, TRUE, FALSE, 0);
  thread = CreateThread(0, 0, start, 0, 0, 0);
  struct {
    LONG status;
    char *teb;
    void *rest[4];
  } basic;
  ((LONG(NTAPI *)(HANDLE, int, void *, ULONG, ULONG *))GetProcAddress(
      GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread"))(
      thread, 0, &basic, sizeof(basic), 0);
  while (!*(void *volatile *)(basic.teb + 0x58))
    SwitchToThread();
  return TRUE;
}
#elif defined(DLLC)
extern int table[8];
static _Thread_local const int *c_word = &table[5];
const int *c_value(void) { return c_word; }
#elif defined(DLL)
extern "C" int table[8];
extern "C" const int *c_value(void);
extern "C" __declspec(dllimport) const void **dep_during(
    void (*)(const void **));

thread_local const int *plain = &table[3];
constinit thread_local const int *constant = &table[4];
inline thread_local const int *vague = &table[6];
thread_local const int *zero = &table[0];
thread_local const int *stored = &table[7];
thread_local void *(*allocate)(size_t) = &::operator new;
thread_local const int *more[2] = {&table[1], &table[2]};

extern "C" __declspec(dllexport) void values(const void **out) {
  const void *v[NVALUES] = {plain,   constant, vague,   zero,   c_value(),
                            stored, (void *)allocate, more[0], more[1]};
  for (int i = 0; i != NVALUES; ++i)
    out[i] = v[i];
}
extern "C" __declspec(dllexport) const void **during(void) {
  return dep_during(values);
}
extern "C" __declspec(dllexport) const int *base(void) { return table; }
// The loading thread's own store, before the fill.
static void NTAPI callback(void *, DWORD reason, void *) {
  if (reason == DLL_PROCESS_ATTACH)
    stored = nullptr;
}
__attribute__((used, section(".CRT$XLB"))) static const PIMAGE_TLS_CALLBACK
    attach = callback;
#elif defined(PLAIN)
extern "C" int table[8];
extern "C" __declspec(dllexport) const int *plain_word = &table[3];
#elif defined(SOLO)
extern "C" int table[8];
thread_local const int *solo = &table[3];
extern "C" __declspec(dllexport) const int *solo_value(void) { return solo; }
#elif defined(START)
// A DLL that the executable loads as it starts, whose attach starts a thread
// that reads the executable's words and the DLL's, and stores to one of the
// executable's in the main thread, before the executable's fill.
extern "C" __declspec(dllimport) void values(const void **);
static const void *seen[NVALUES + 1];
static DWORD WINAPI start(void *) {
  values(seen);
  seen[NVALUES] =
      ((const void *(*)())GetProcAddress(GetModuleHandleA(0), "exe_word"))();
  return 0;
}
static HANDLE thread;
extern "C" __declspec(dllexport) const void **start_seen(void) {
  WaitForSingleObject(thread, INFINITE);
  return seen;
}
static void CALLBACK slow(ULONG_PTR) { Sleep(100); }
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, void *) {
  if (reason == DLL_PROCESS_ATTACH) {
    ((void (*)())GetProcAddress(GetModuleHandleA(0), "exe_store"))();
    thread = CreateThread(0, 0, start, 0, 0, 0);
    // The main thread reaches the executable's entry after this.
    HANDLE main;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                    GetCurrentProcess(), &main, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    QueueUserAPC(slow, main, 0);
  }
  return TRUE;
}
#elif defined(STATIC)
#include <stdio.h>
extern int table[8];
__declspec(dllimport) void values(const void **);
__declspec(dllimport) const void **start_seen(void);
static _Thread_local const int *word = &table[2];
static _Thread_local const int *stored = &table[1];
__declspec(dllexport) const void *exe_word(void) { return word; }
__declspec(dllexport) void exe_store(void) { stored = 0; }
static int right(const void *const *v) {
  int n = 0;
  for (int i = 0; i != NVALUES; ++i) {
    static const int index[NVALUES] = {3, 4, 6, 0, 5, -1, -2, 1, 2};
    n += index[i] >= 0 ? v[i] == &table[index[i]] : index[i] == -1 || v[i];
  }
  return n;
}
int main(void) {
  const void *v[NVALUES];
  values(v);
  printf("main: right %d, exe %d, own store %d\n", right(v), word == &table[2],
         !stored);
  const void **seen = start_seen();
  printf("started: right %d, exe %d\n", right(seen), seen[NVALUES] == &table[2]);
  return 0;
}
#else
#include <stdio.h>
static int replaced;
void *operator new(size_t n) {
  ++replaced;
  return HeapAlloc(GetProcessHeap(), 0, n ? n : 1);
}

static void (*values)(const void **);
static const int *table;

static int right(const void *const *v, int own) {
  static const int index[NVALUES] = {3, 4, 6, 0, 5, 7, -1, 1, 2};
  int n = 0;
  for (int i = 0; i != NVALUES; ++i)
    n += index[i] < 0   ? v[i] == (void *)static_cast<void *(*)(size_t)>(
                                       &::operator new)
         : own && i == 5 ? !v[i]
                         : v[i] == &table[index[i]];
  return n;
}
static HANDLE go;
static DWORD WINAPI running(void *out) {
  WaitForSingleObject(go, INFINITE);
  values((const void **)out);
  return 0;
}
static HMODULE load(const char *path) {
  HMODULE dll = LoadLibraryA(path);
  if (dll) {
    values = (void (*)(const void **))GetProcAddress(dll, "values");
    table = ((const int *(*)())GetProcAddress(dll, "base"))();
  }
  return dll;
}
static DWORD WINAPI idle(void *stop) {
  return WaitForSingleObject(stop, INFINITE);
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  go = CreateEventA(0, TRUE, FALSE, 0);
  const void *v[4][NVALUES];
  HANDLE threads[2];
  for (int i = 0; i != 2; ++i)
    threads[i] = CreateThread(0, 0, running, v[i], 0, 0);
  Sleep(50);
  HMODULE dll = load(argv[1]);
  values(v[2]);
  printf("loading: right %d, own store %d\n", right(v[2], 1), !v[2][5]);
  SetEvent(go);
  WaitForMultipleObjects(2, threads, TRUE, INFINITE);
  printf("running: right %d\nrunning: right %d\n", right(v[0], 0),
         right(v[1], 0));
  const void **during =
      ((const void **(*)())GetProcAddress(dll, "during"))();
  printf("during: right %d\n", right(during, 0));
  HANDLE after = CreateThread(0, 0, running, v[3], 0, 0);
  WaitForSingleObject(after, INFINITE);
  printf("after: right %d\n", right(v[3], 0));

  // A thread whose rights deny every caller, which ends itself: one ended
  // from outside may hold the loader's lock, which its attach takes.
  HANDLE stop = CreateEventA(0, TRUE, FALSE, 0);
  SECURITY_DESCRIPTOR deny;
  InitializeSecurityDescriptor(&deny, SECURITY_DESCRIPTOR_REVISION);
  ACL empty;
  InitializeAcl(&empty, sizeof(empty), ACL_REVISION);
  SetSecurityDescriptorDacl(&deny, TRUE, &empty, FALSE);
  SECURITY_ATTRIBUTES attributes = {sizeof(attributes), &deny, FALSE};
  HANDLE unreachable = CreateThread(&attributes, 0, idle, stop, 0, 0);
  dll = LoadLibraryA(argv[2]);
  printf("unreachable: %lu\n", dll ? 0 : GetLastError());
  SetEvent(stop);
  WaitForSingleObject(unreachable, INFINITE);

  ImpersonateSelf(SecurityImpersonation);
  dll = LoadLibraryA(argv[2]);
  HANDLE token = 0;
  int still = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token);
  printf("impersonating: loaded %d, still %d, right %d\n", !!dll, still,
         dll && ((const int *(*)())GetProcAddress(dll, "solo_value"))() ==
                    &table[3]);
  return 0;
}
#endif
