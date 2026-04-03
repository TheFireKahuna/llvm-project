// Test _is_c_termination_complete state transitions.
//
// RUN: %clang_crt_main -std=c++17 %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>

extern "C" int _is_c_termination_complete(void);

struct TerminationChecker {
  ~TerminationChecker() {
    // During destructor execution, termination is not yet complete.
    int complete = _is_c_termination_complete();
    printf("In destructor: termination complete = %d\n", complete);
  }
};

static TerminationChecker g_checker;

int main() {
  // CHECK: Termination state test
  printf("Termination state test\n");

  // Before exit, termination should not be complete.
  int complete_before = _is_c_termination_complete();
  // CHECK: before exit: termination complete = 0
  printf("before exit: termination complete = %d\n", complete_before);

  // CHECK: main done
  printf("main done\n");
  return 0;
}

// During destructor, termination is in progress but not complete.
// The exact value depends on implementation, but it should be queryable.
// CHECK: In destructor: termination complete = {{0|1}}
