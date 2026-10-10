// RUN: %clang_wincrt %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s

// At exit, atexit, _onexit and __cxa_atexit registrations run in exact
// reverse order, interleaved; one registered while they run runs next; the
// .CRT$XP and .CRT$XT tables run after all of them.

#include <stdio.h>
#include <stdlib.h>

int __cxa_atexit(void (*)(void *), void *, void *);
int _is_c_termination_complete(void);
extern void *__dso_handle;

static void say(void *Text) { printf("%s\n", (const char *)Text); }
static void first(void) { say("atexit 1"); }
static int second(void) {
  say("_onexit 2");
  return 0;
}
static void late(void) { say("registered during exit"); }
static void fourth(void) {
  say("atexit 4");
  atexit(late);
}
static void fromConstructor(void) { say("registered by a constructor"); }

static void construct(void) { atexit(fromConstructor); }
static void preterminate(void) {
  printf("pre-terminator, complete %d\n", _is_c_termination_complete());
}
static void terminate(void) { say("terminator"); }

#pragma section(".CRT$XCU", read)
#pragma section(".CRT$XPU", read)
#pragma section(".CRT$XTU", read)
__attribute__((used))
__declspec(allocate(".CRT$XCU")) static void (*const Constructor)(void) =
    construct;
__attribute__((used))
__declspec(allocate(".CRT$XPU")) static void (*const Preterminator)(void) =
    preterminate;
__attribute__((used))
__declspec(allocate(".CRT$XTU")) static void (*const Terminator)(void) =
    terminate;

int main(void) {
  atexit(first);
  _onexit(second);
  __cxa_atexit(say, "__cxa_atexit 3", &__dso_handle);
  atexit(fourth);
  __cxa_atexit(say, "__cxa_atexit 5", &__dso_handle);
  return 0;
}

// CHECK:      __cxa_atexit 5
// CHECK-NEXT: atexit 4
// CHECK-NEXT: registered during exit
// CHECK-NEXT: __cxa_atexit 3
// CHECK-NEXT: _onexit 2
// CHECK-NEXT: atexit 1
// CHECK-NEXT: registered by a constructor
// CHECK-NEXT: pre-terminator, complete 0
// CHECK-NEXT: terminator
// CHECK-NOT:  {{.}}
