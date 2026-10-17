// DEFINE: %{cxx} = %clang_wincrt -std=c++17 -fno-exceptions
// RUN: rm -rf %t && mkdir %t
// RUN: %{cxx} -c -DDEFINE %s -o %t/define.o
// RUN: %{cxx} -c -DUNMARKED -fauto-import %s -o %t/unmarked.o
// RUN: %{cxx} -c -DMARKED %s -o %t/marked.o
// RUN: %{cxx} %t/define.o %t/unmarked.o %t/marked.o -o %t/main.exe
// RUN: %run %t/main.exe | FileCheck %s
// RUN: %{cxx} -flto -c -DDEFINE %s -o %t/define.bc
// RUN: %{cxx} -flto -c -DUNMARKED -fauto-import %s -o %t/unmarked.bc
// RUN: %{cxx} -flto -c -DMARKED %s -o %t/marked.bc
// RUN: %{cxx} -flto %t/define.bc %t/unmarked.bc %t/marked.bc -o %t/lto.exe
// RUN: %run %t/lto.exe | FileCheck %s
// RUN: %{cxx} -flto %t/define.bc %t/unmarked.o %t/marked.o -o %t/mixed.exe
// RUN: %run %t/mixed.exe | FileCheck %s

// A thread-local variable that a unit defines without exporting it is reached
// from another unit of the image through the record the linker writes for it,
// by a declaration under -fauto-import, which may name another image's
// variable, or by one marked as crossing the shared-library boundary. The
// record names the byte that guards the variable's initialization on each
// thread, which the marked declaration tests, so that the variable is
// initialized once on each thread, whichever unit reaches it first there,
// with or without LTO.

// CHECK: main: same 1 value 1 inits 1
// CHECK: thread: same 1 value 2 inits 1

#include <stdio.h>
#include <windows.h>

#define API __attribute__((visibility("default")))

int next();
int inits();

#if defined(DEFINE)
static int Next, Inits;
int next() { return ++Next; }
int inits() { return Inits; }
static int initialize() {
  ++Inits;
  return next();
}
thread_local int counter = initialize();
int *defined() { return &counter; }
#elif defined(UNMARKED)
extern thread_local int counter;
int *unmarked() { return &counter; }
#elif defined(MARKED)
API extern thread_local int counter;
int *defined();
int *unmarked();

static void report(const char *who) {
  int before = inits();
  // The marked declaration's access comes first, so that it runs the
  // initialization through the guard its record names.
  int *marked = &counter;
  printf("%s: same %d value %d inits %d\n", who,
         marked == unmarked() && marked == defined(), *marked,
         inits() - before);
}

static DWORD WINAPI thread(void *) {
  report("thread");
  return 0;
}

int main() {
  report("main");
  HANDLE t = CreateThread(nullptr, 0, thread, nullptr, 0, nullptr);
  WaitForSingleObject(t, INFINITE);
  return 0;
}
#endif
