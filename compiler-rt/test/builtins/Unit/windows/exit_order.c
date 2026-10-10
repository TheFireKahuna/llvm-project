// RUN: %clang_wincrt -shared -DDLL %s -o %t.dll
// RUN: %clang_wincrt %s -o %t.exe
// RUN: %run %t.exe %t.dll return | FileCheck %s --check-prefixes=CHECK,MAIN
// RUN: %run %t.exe %t.dll exit | FileCheck %s --check-prefixes=CHECK,MAIN
// RUN: %run %t.exe %t.dll thread | FileCheck %s --check-prefixes=CHECK,THREAD
// RUN: %run %t.exe %t.dll dll | FileCheck %s --check-prefixes=CHECK,MAIN

// Returning from main and calling exit, from main, from another thread or
// from a DLL, end the program in one order: the exiting thread's thread-local
// destructors, then the atexit and __cxa_atexit registrations of every image
// in reverse order, then the DLLs' detach, with buffered output flushed after
// all of them.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

int __cxa_atexit(void (*)(void *), void *, void *);
int __cxa_thread_atexit_impl(void (*)(void *), void *, void *);
extern void *__dso_handle;

static void say(void *Text) { printf("%s\n", (const char *)Text); }

#ifdef DLL

static void dllAtexit(void) { say("dll atexit"); }

__declspec(dllexport) void registerInDll(void) {
  __cxa_atexit(say, "dll __cxa_atexit", &__dso_handle);
  atexit(dllAtexit);
}

__declspec(dllexport) void exitFromDll(int Code) { exit(Code); }

BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, LPVOID Reserved) {
  // Without a newline, for the C runtime to flush.
  if (Reason == DLL_PROCESS_DETACH)
    printf("dll detach");
  return TRUE;
}

#else

static void firstAtexit(void) { say("first atexit"); }
static void lastAtexit(void) { say("last atexit"); }
static void construct(void) { __cxa_atexit(say, "static", &__dso_handle); }

#pragma section(".CRT$XCU", read)
__attribute__((used))
__declspec(allocate(".CRT$XCU")) static void (*const Constructor)(void) =
    construct;

static DWORD WINAPI thread(LPVOID Parameter) {
  (void)Parameter;
  __cxa_thread_atexit_impl(say, "second thread's thread_local", &__dso_handle);
  exit(0);
}

int main(int argc, char **argv) {
  atexit(firstAtexit);
  HMODULE Module = LoadLibraryA(argv[1]);
  ((void (*)(void))GetProcAddress(Module, "registerInDll"))();
  __cxa_thread_atexit_impl(say, "main thread_local", &__dso_handle);
  atexit(lastAtexit);
  if (!strcmp(argv[2], "exit"))
    exit(0);
  if (!strcmp(argv[2], "thread")) {
    CreateThread(NULL, 0, thread, NULL, 0, NULL);
    Sleep(INFINITE);
  }
  if (!strcmp(argv[2], "dll"))
    ((void (*)(int))GetProcAddress(Module, "exitFromDll"))(0);
  return 0;
}

#endif

// MAIN:        main thread_local
// THREAD:      second thread's thread_local
// CHECK-NEXT:  last atexit
// CHECK-NEXT:  dll atexit
// CHECK-NEXT:  dll __cxa_atexit
// CHECK-NEXT:  first atexit
// CHECK-NEXT:  static
// CHECK-NEXT:  dll detach
// CHECK-NOT:   {{.}}
