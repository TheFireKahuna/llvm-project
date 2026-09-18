// Immediate and quick exit must bypass C++ destruction, including TLS in the
// shared ABI runtime. Keep this check when repairing loader TLS callbacks.
// RUN: %clangxx_crt_main -std=c++17 -O2 %s -o %t.exe
// RUN: %run %t.exe immediate 2>&1 | FileCheck %s --implicit-check-not=destroy --implicit-check-not=atexit
// RUN: %run %t.exe quick 2>&1 | FileCheck %s --implicit-check-not=destroy --implicit-check-not=atexit
// REQUIRES: windows, crt

#include <stdio.h>
#include <stdlib.h>

struct Object {
  int id;
  explicit Object(int value) : id(value) {}
  ~Object() { fprintf(stderr, "destroy %d\n", id); }
};

static Object global(1);
thread_local Object local(2);
static void onExit() { fprintf(stderr, "atexit\n"); }

int main(int argc, char **argv) {
  if (argc != 2 || atexit(onExit) != 0)
    return 1;
  fprintf(stderr, "ready: global %d, TLS %d\n", global.id, local.id);
  if (argv[1][0] == 'q')
    quick_exit(0);
  _Exit(0);
}

// CHECK: ready: global 1, TLS 2
