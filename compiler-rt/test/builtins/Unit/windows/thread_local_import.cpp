// RUN: %clang_wincrt -std=c++20 -fno-exceptions -shared -DDLL %s -o %t.dll
// RUN: %clang_wincrt -std=c++20 -fno-exceptions %s %t.dll.lib -o %t.exe
// RUN: %run %t.exe | FileCheck %s
// RUN: llvm-readobj --coff-exports %t.dll | FileCheck --check-prefix=EXPORT %s
// RUN: %clang_wincrt -std=c++20 -fno-exceptions -flto %s %t.dll.lib \
// RUN:   -o %t.lto.exe
// RUN: %run %t.lto.exe | FileCheck %s
// RUN: %clang_wincrt -std=c++20 -fno-exceptions -flto -shared -DDLL %s \
// RUN:   -o %t.lto.dll
// RUN: %clang_wincrt -std=c++20 -fno-exceptions %s %t.lto.dll.lib \
// RUN:   -o %t.with-lto-dll.exe
// RUN: %run %t.with-lto-dll.exe | FileCheck %s

// An executable reads, writes and takes the address of a DLL's thread-local
// variables through the records the DLL exports in their place: one with
// constant initialization, one initialized dynamically on each thread's first
// access, one whose destructor runs at each thread's exit, and one declared
// constinit, which needs no guard. Each thread has its own instances, shared
// with the DLL's code.

// CHECK: main: plain 7 dynamic 1 counter 2 fixed 42
// CHECK: thread: plain 7 dynamic 3 counter 4 fixed 42
// CHECK: destroyed 1
// CHECK: done

// EXPORT-NOT: Name: plain{{$}}
// EXPORT:     Name: _ZTH5plain
// EXPORT:     Name: plain$tls

#include <stdio.h>
#include <windows.h>

#define API __attribute__((visibility("default")))

struct Counter {
  int value;
  Counter();
  ~Counter();
};
API extern thread_local int plain;
API extern thread_local int dynamic;
API extern thread_local Counter counter;
API extern constinit thread_local int fixed;
API int readPlainInDll();
API int *addressOfPlainInDll();
API int destroyed();

#ifdef DLL

// The C++ runtime's entry point for thread-local destructors, which this test
// does not link.
extern "C" int __llvm_kcfi_cxa_thread_atexit_impl(void (*)(void *), void *,
                                                  void *);
extern "C" int __llvm_kcfi_cxa_thread_atexit(void (*Destructor)(void *),
                                             void *Object, void *Handle) {
  return __llvm_kcfi_cxa_thread_atexit_impl(Destructor, Object, Handle);
}

static int Next, Destroyed;
static int next() { return ++Next; }
Counter::Counter() : value(next()) {}
Counter::~Counter() { ++Destroyed; }
thread_local int plain = 7;
thread_local int dynamic = next();
thread_local Counter counter;
constinit thread_local int fixed = 42;
int readPlainInDll() { return plain; }
int *addressOfPlainInDll() { return &plain; }
int destroyed() { return Destroyed; }

#else

// The DLL initializes the variables of a thread in order on its first access
// to either.
static void report(const char *Who) {
  int Plain = plain, Dynamic = dynamic, Value = counter.value, Fixed = fixed;
  printf("%s: plain %d dynamic %d counter %d fixed %d\n", Who, Plain, Dynamic,
         Value, Fixed);
  fflush(stdout);
}

static DWORD WINAPI thread(LPVOID) {
  report("thread");
  plain = 9;
  return readPlainInDll() == 9 && &plain == addressOfPlainInDll() ? 0 : 1;
}

int main() {
  report("main");
  HANDLE Thread = CreateThread(nullptr, 0, thread, nullptr, 0, nullptr);
  WaitForSingleObject(Thread, INFINITE);
  DWORD Code = 1;
  GetExitCodeThread(Thread, &Code);
  if (Code != 0 || plain != 7 || &plain != addressOfPlainInDll())
    return 1;
  printf("destroyed %d\n", destroyed());
  printf("done\n");
  return 0;
}

#endif
