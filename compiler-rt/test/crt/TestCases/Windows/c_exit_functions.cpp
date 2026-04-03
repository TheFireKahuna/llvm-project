// Test _cexit and _c_exit functions.
//
// RUN: %clang_crt_main -std=c++17 %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>

extern "C" void _cexit(void);
extern "C" void _c_exit(void);

static int g_destructor_called = 0;

struct CleanupObject {
  int id;
  CleanupObject(int i) : id(i) {}
  ~CleanupObject() {
    g_destructor_called++;
    printf("CleanupObject(%d) destructed\n", id);
  }
};

static CleanupObject obj1(1);
static CleanupObject obj2(2);

int main() {
  // CHECK: _cexit/_c_exit test
  printf("_cexit/_c_exit test\n");

  // Call _c_exit first - should be a no-op per MSVC semantics.
  // No destructors, no buffer flush.
  _c_exit();
  // CHECK: after _c_exit: destructors called = 0
  printf("after _c_exit: destructors called = %d\n", g_destructor_called);

  // _cexit runs all cleanup but does not terminate.
  // Note: calling _cexit manually is unusual - normally exit() does this.
  // After _cexit, destructors will have run.
  printf("calling _cexit\n");
  // CHECK: calling _cexit
  _cexit();

  // CHECK: CleanupObject(2) destructed
  // CHECK: CleanupObject(1) destructed

  // After _cexit, cleanup has run.
  // CHECK: after _cexit: destructors called = 2
  printf("after _cexit: destructors called = %d\n", g_destructor_called);

  // Calling _cexit again should be a no-op (double-finalization prevention).
  _cexit();
  // CHECK: after second _cexit: destructors called = 2
  printf("after second _cexit: destructors called = %d\n", g_destructor_called);

  // CHECK: main done
  printf("main done\n");

  // Note: returning from main after _cexit has already run cleanup.
  // Normal exit() will try to run cleanup again but should be guarded.
  return 0;
}
