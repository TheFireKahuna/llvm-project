// RUN: %clang_wincrt %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
// RUN: %clang_wincrt -DNO_THREAD_LOCALS %s -o %t-plain.exe
// RUN: llvm-readobj --file-headers %t-plain.exe | FileCheck %s --check-prefix=PLAIN

// A thread's thread-local destructors run in reverse order when it exits.
// On exit, the exiting thread's run before any static destructor, even one
// registered after them, and before a function that another runtime put in
// the Universal CRT's atexit table after wincrt's own entry. A program that uses atexit but registers no
// thread-local destructor links no registry for them, and so gets no TLS
// directory.

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#ifdef NO_THREAD_LOCALS

static void nothing(void) {}

int main(void) {
  atexit(nothing);
  return 0;
}

#else

int __cxa_thread_atexit_impl(void (*)(void *), void *, void *);
int _crt_atexit(void (*)(void));
extern void *__dso_handle;

static void say(void *Text) { printf("%s\n", (const char *)Text); }
static void registerDuringDrain(void *Text) {
  say(Text);
  __cxa_thread_atexit_impl(say, "registered while they run", &__dso_handle);
}
static void staticDestructor(void) { say("static destructor"); }
static void otherRuntime(void) { say("another runtime's atexit function"); }

static DWORD WINAPI thread(LPVOID Parameter) {
  (void)Parameter;
  __cxa_thread_atexit_impl(say, "thread 1", &__dso_handle);
  __cxa_thread_atexit_impl(registerDuringDrain, "thread 2", &__dso_handle);
  __cxa_thread_atexit_impl(say, "thread 3", &__dso_handle);
  return 0;
}

int main(void) {
  HANDLE Thread = CreateThread(NULL, 0, thread, NULL, 0, NULL);
  WaitForSingleObject(Thread, INFINITE);
  CloseHandle(Thread);
  printf("thread joined\n");
  __cxa_thread_atexit_impl(say, "main thread", &__dso_handle);
  atexit(staticDestructor);
  _crt_atexit(otherRuntime);
  return 0;
}

#endif

// CHECK:      thread 3
// CHECK-NEXT: thread 2
// CHECK-NEXT: registered while they run
// CHECK-NEXT: thread 1
// CHECK-NEXT: thread joined
// CHECK-NEXT: main thread
// CHECK-NEXT: another runtime's atexit function
// CHECK-NEXT: static destructor

// PLAIN: TLSTableSize: 0x0
