// RUN: %clang -shared -DDLL %s -o %t.dll
// RUN: %clang -shared -DDLL -DNO_DLLMAIN %s -o %t-default.dll
// RUN: %clang %s -o %t.exe
// RUN: %run %t.exe %t.dll unload | FileCheck %s --check-prefixes=DLLMAIN,CHECK
// RUN: %run %t.exe %t-default.dll unload | FileCheck %s --check-prefixes=DEFAULT,CHECK
// RUN: env WINCRT_FAIL_ATTACH=1 %run %t.exe %t.dll unload | FileCheck %s --check-prefix=FAIL
// RUN: %run %t.exe %t.dll pin | FileCheck %s --check-prefix=PIN
// RUN: %run %t.exe %t.dll thread-local | FileCheck %s --check-prefix=TLS

// [DLLMAIN] _DllMainCRTStartup runs a DLL's constructors before its DllMain
// and passes every notification on to it. [CHECK] Unloading runs only the
// DLL's own registrations, in reverse order, after its DllMain and before its
// terminators; the executable's run at exit. [DEFAULT] A DLL that defines no
// DllMain gets one. [FAIL] A failed attach is undone once. [PIN] A DLL that
// registers the destructor of an object outside itself, as the image that
// initialized a shared instance does, stays loaded, and the destructor runs at
// exit in the one order. [TLS] A pending thread-local destructor keeps its DLL
// loaded, past its last FreeLibrary, until it has run.

// DLLMAIN:      process attach, constructed 1
// DLLMAIN-NEXT: constructed 1
// DLLMAIN:      thread attach
// DLLMAIN:      thread detach
// DLLMAIN:      process detach
// DEFAULT:      constructed 1
// CHECK-NEXT:   dll atexit 2
// CHECK-NEXT:   dll atexit 1
// CHECK-NEXT:   dll terminator
// CHECK-NEXT:   unloaded 1
// CHECK-NEXT:   exe atexit 2
// CHECK-NEXT:   exe atexit 1

// FAIL:      process attach, constructed 1
// FAIL-NEXT: process detach
// FAIL-NEXT: dll terminator
// FAIL-NEXT: load failed

// PIN:      unloaded 0
// PIN-NEXT: exe atexit 2
// PIN-NEXT: dll destroys the executable's object
// PIN-NEXT: exe atexit 1
// PIN-NEXT: process detach
// PIN-NEXT: dll terminator

// TLS:      unloaded 0
// TLS:      dll destroys a thread_local
// TLS:      process detach
// TLS-NEXT: dll terminator
// TLS-NEXT: unloaded 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

int __cxa_atexit(void (*)(void *), void *, void *);
int __cxa_thread_atexit_impl(void (*)(void *), void *, void *);
extern void *__dso_handle;

#ifdef DLL

static int Constructed;
static void construct(void) { Constructed = 1; }
static void terminate(void) { printf("dll terminator\n"); }
static void dll1(void) { printf("dll atexit 1\n"); }
static void dll2(void) { printf("dll atexit 2\n"); }
static void destroy(void *Object) { printf("dll destroys %s\n", (char *)Object); }

__attribute__((used, section(".CRT$XCU"))) static void (*const C)(void) = construct;
__attribute__((used, section(".CRT$XTU"))) static void (*const T)(void) = terminate;

__declspec(dllexport) int constructed(void) { return Constructed; }
__declspec(dllexport) void registerInDll(void *Object) {
  if (Object) {
    __cxa_atexit(destroy, Object, &__dso_handle);
    return;
  }
  atexit(dll1);
  atexit(dll2);
}
__declspec(dllexport) void registerThreadLocal(void) {
  __cxa_thread_atexit_impl(destroy, "a thread_local", &__dso_handle);
}

#  ifndef NO_DLLMAIN
BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, LPVOID Reserved) {
  static const char *const Names[] = {"process detach", "process attach, constructed",
                                      "thread attach", "thread detach"};
  printf(Reason == DLL_PROCESS_ATTACH ? "%s %d\n" : "%s\n", Names[Reason], Constructed);
  return Reason != DLL_PROCESS_ATTACH || !getenv("WINCRT_FAIL_ATTACH");
}
#  endif

#else

static char Shared[] = "the executable's object";
static HMODULE Module;
static HANDLE Registered, Unloaded;

static FARPROC get(const char *Name) { return GetProcAddress(Module, Name); }
static void exe1(void) { printf("exe atexit 1\n"); }
static void exe2(void) { printf("exe atexit 2\n"); }

static DWORD WINAPI thread(LPVOID ThreadLocal) {
  if (ThreadLocal) {
    ((void (*)(void))get("registerThreadLocal"))();
    SetEvent(Registered);
    WaitForSingleObject(Unloaded, INFINITE);
  }
  return 0;
}

int main(int argc, char **argv) {
  int ThreadLocal = !strcmp(argv[2], "thread-local");
  atexit(exe1);
  Module = LoadLibraryA(argv[1]);
  if (!Module)
    return printf("load failed\n"), 0;
  printf("constructed %d\n", ((int (*)(void))get("constructed"))());
  Registered = CreateEventW(NULL, TRUE, FALSE, NULL);
  Unloaded = CreateEventW(NULL, TRUE, FALSE, NULL);
  HANDLE Thread = CreateThread(NULL, 0, thread, (LPVOID)(INT_PTR)ThreadLocal, 0, NULL);
  WaitForSingleObject(ThreadLocal ? Registered : Thread, INFINITE);
  if (!ThreadLocal)
    ((void (*)(void *))get("registerInDll"))(!strcmp(argv[2], "pin") ? Shared : NULL);
  atexit(exe2);
  FreeLibrary(Module);
  printf("unloaded %d\n", GetModuleHandleA(argv[1]) == NULL);
  if (ThreadLocal) {
    SetEvent(Unloaded);
    WaitForSingleObject(Thread, INFINITE);
    // The thread pool drops the last reference.
    for (int I = 0; I < 500 && GetModuleHandleA(argv[1]); ++I)
      Sleep(10);
    printf("unloaded %d\n", GetModuleHandleA(argv[1]) == NULL);
  }
  return 0;
}

#endif
