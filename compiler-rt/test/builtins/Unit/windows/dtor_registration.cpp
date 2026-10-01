// RUN: %clang_wincrt -fno-exceptions -fno-threadsafe-statics %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s

// Clang registers the destructors of static objects, and the helpers that
// destroy arrays, through wincrt's __llvm_kcfi_cxa_atexit, since they carry
// the salted destructor type; C code passes __cxa_atexit a plain function.
// Both kinds run at exit, each called through its own type, in exact reverse
// order of registration. The same holds for thread-local destructors, which
// the C++ runtime registers through __llvm_kcfi_cxa_thread_atexit_impl, and
// for one registered while they run into the slot a salted one left.

#include <stdio.h>
#include <stdlib.h>

extern "C" {
int __cxa_atexit(void (*)(void *), void *, void *);
int __cxa_thread_atexit_impl(void (*)(void *), void *, void *);
int __llvm_kcfi_cxa_thread_atexit_impl(void (*)(void *), void *, void *);
extern void *__dso_handle;
}

// The type clang gives destructors; the salt is part of the function type.
#pragma clang diagnostic ignored "-Wignored-attributes"
#define SALTED __attribute__((cfi_salt("__cxa_dtor")))
typedef void (*Destructor)(void *);

static void say(void *Text) { printf("%s\n", (const char *)Text); }

static void saySalted(void *Text) SALTED { say(Text); }

static void registerDuringDrain(void *Text) SALTED {
  say(Text);
  __cxa_thread_atexit_impl(say, (void *)"thread, registered while they run",
                           &__dso_handle);
}

static int registerPlain(const char *Text) {
  return __cxa_atexit(say, (void *)Text, &__dso_handle);
}

struct Object {
  const char *Name;
  ~Object() { say((void *)Name); }
};

Object First = {"static object 1"};
int Second = registerPlain("__cxa_atexit 2");
Object Third[2] = {{"array 3, element 0"}, {"array 3, element 1"}};

static void atexitSix(void) { say((void *)"atexit 6"); }

static void useLocal(void) { static Object Local = {"static local 5"}; }

int main(void) {
  registerPlain("__cxa_atexit 4");
  useLocal();
  atexit(atexitSix);
  __cxa_thread_atexit_impl(say, (void *)"thread 1", &__dso_handle);
  __llvm_kcfi_cxa_thread_atexit_impl(
      reinterpret_cast<Destructor>(registerDuringDrain), (void *)"thread 2",
      &__dso_handle);
  __llvm_kcfi_cxa_thread_atexit_impl(reinterpret_cast<Destructor>(saySalted),
                                     (void *)"thread 3", &__dso_handle);
  return 0;
}

// CHECK:      thread 3
// CHECK-NEXT: thread 2
// CHECK-NEXT: thread, registered while they run
// CHECK-NEXT: thread 1
// CHECK-NEXT: atexit 6
// CHECK-NEXT: static local 5
// CHECK-NEXT: __cxa_atexit 4
// CHECK-NEXT: array 3, element 1
// CHECK-NEXT: array 3, element 0
// CHECK-NEXT: __cxa_atexit 2
// CHECK-NEXT: static object 1
// CHECK-NOT:  {{.}}
