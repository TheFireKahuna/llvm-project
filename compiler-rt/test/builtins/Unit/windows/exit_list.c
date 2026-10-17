// RUN: rm -rf %t && mkdir %t %t/dup
// RUN: %clang_wincrt -shared -DSHARED_DLL %s -o %t/shared.dll
// RUN: %clang_wincrt -static -shared -DSTATIC_DLL %s -o %t/static.dll
// RUN: %clang_wincrt %s -o %t/main.exe
// RUN: %clang_wincrt -static %s -o %t/smain.exe
// RUN: cp %wincrt_libdir/clang_rt.wincrt_dynamic.dll %t/dup/registry-copy.dll
// RUN: %run %t/main.exe %t/dup/registry-copy.dll | FileCheck %s
// RUN: %run %t/smain.exe %t/dup/registry-copy.dll | FileCheck %s
// RUN: %run %t/main.exe %t/dup/registry-copy.dll exitprocess | \
// RUN:   FileCheck --check-prefix=ABRUPT %s

// Every copy of the termination registries in a process uses the one exit
// list that the executable reserves: the executable's (the shared runtime
// DLL's, or its own under -static), a DLL's own under -static, and a second
// runtime DLL loaded by full path. Exit runs every registration in exact
// reverse order across them, under one token, and a thread's thread-local
// destructors run in exact reverse order across them when it exits.
// ExitProcess runs no registration.

// CHECK:      thread static.dll
// CHECK-NEXT: thread shared.dll
// CHECK-NEXT: thread main
// CHECK-NEXT: main 3
// CHECK-NEXT: copy 1
// CHECK-NEXT: shared.dll 1
// CHECK-NEXT: main 2
// CHECK-NEXT: static.dll 1
// CHECK-NEXT: main 1
// CHECK-NOT:  {{.}}

// The thread exits before ExitProcess, which runs no registration.
// ABRUPT:      thread static.dll
// ABRUPT-NEXT: thread shared.dll
// ABRUPT-NEXT: thread main
// ABRUPT-NOT:  {{.}}

#include <stdio.h>
#include <string.h>
#include <windows.h>

#define API __attribute__((visibility("default")))

extern void *__dso_handle;
int __cxa_atexit(void (*)(void *), void *, void *);
int __cxa_thread_atexit_impl(void (*)(void *), void *, void *);

static void say(void *Text) {
  printf("%s\n", (const char *)Text);
  fflush(stdout);
}

#if defined(SHARED_DLL) || defined(STATIC_DLL)
#  ifdef SHARED_DLL
#    define NAME "shared.dll"
#  else
#    define NAME "static.dll"
#  endif
API void registerStatic(void) { __cxa_atexit(say, NAME " 1", &__dso_handle); }
API void registerThread(void) {
  __cxa_thread_atexit_impl(say, "thread " NAME, &__dso_handle);
}
#else
typedef void (*Register)(void);
typedef int (*CxaAtexit)(void (*)(void *), void *, void *);

static Register get(HMODULE Module, const char *Name) {
  return (Register)GetProcAddress(Module, Name);
}

static DWORD WINAPI thread(LPVOID Arg) {
  HMODULE *Modules = Arg;
  __cxa_thread_atexit_impl(say, "thread main", &__dso_handle);
  get(Modules[0], "registerThread")();
  get(Modules[1], "registerThread")();
  return 0;
}

int main(int argc, char **argv) {
  char Dir[MAX_PATH], Path[MAX_PATH];
  GetModuleFileNameA(NULL, Dir, MAX_PATH);
  *strrchr(Dir, '\\') = 0;
  HMODULE Modules[2];

  __cxa_atexit(say, "main 1", &__dso_handle);
  snprintf(Path, MAX_PATH, "%s\\static.dll", Dir);
  Modules[1] = LoadLibraryA(Path);
  get(Modules[1], "registerStatic")();
  __cxa_atexit(say, "main 2", &__dso_handle);
  snprintf(Path, MAX_PATH, "%s\\shared.dll", Dir);
  Modules[0] = LoadLibraryA(Path);
  get(Modules[0], "registerStatic")();
  HMODULE Copy = LoadLibraryA(argv[1]);
  ((CxaAtexit)GetProcAddress(Copy, "__cxa_atexit"))(say, "copy 1", NULL);
  __cxa_atexit(say, "main 3", &__dso_handle);

  HANDLE Thread = CreateThread(NULL, 0, thread, Modules, 0, NULL);
  WaitForSingleObject(Thread, INFINITE);
  if (argc > 2)
    ExitProcess(0);
  return 0;
}
#endif
