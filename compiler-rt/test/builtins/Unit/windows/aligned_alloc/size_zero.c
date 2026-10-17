// RUN: %clang_wincrt %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s

// A request for no bytes gets a unique, aligned block that free takes, as
// malloc(0) gets a unique block.

#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void check(size_t Alignment) {
  void *P = aligned_alloc(Alignment, 0);
  void *Q = aligned_alloc(Alignment, 0);
  void *R = NULL;
  int Error = posix_memalign(
      &R, Alignment < sizeof(void *) ? sizeof(void *) : Alignment, 0);
  printf("%zu: %s %s %s %s\n", Alignment, P && Q && !Error ? "blocks" : "null",
         P != Q && P != R && Q != R ? "unique" : "same",
         (uintptr_t)P % Alignment || (uintptr_t)Q % Alignment ||
                 (uintptr_t)R % Alignment
             ? "misaligned"
             : "aligned",
         _msize(P) && _msize(Q) && _msize(R) ? "sized" : "empty");
  free(P);
  free(Q);
  free(R);
}

int main(void) {
  for (size_t A = 1; A <= 2 * 1024 * 1024; A *= 2)
    check(A);
  return 0;
}

// CHECK:      1: blocks unique aligned sized
// CHECK-NEXT: 2: blocks unique aligned sized
// CHECK-NEXT: 4: blocks unique aligned sized
// CHECK-NEXT: 8: blocks unique aligned sized
// CHECK-NEXT: 16: blocks unique aligned sized
// CHECK-NEXT: 32: blocks unique aligned sized
// CHECK-NEXT: 64: blocks unique aligned sized
// CHECK-NEXT: 128: blocks unique aligned sized
// CHECK-NEXT: 256: blocks unique aligned sized
// CHECK-NEXT: 512: blocks unique aligned sized
// CHECK-NEXT: 1024: blocks unique aligned sized
// CHECK-NEXT: 2048: blocks unique aligned sized
// CHECK-NEXT: 4096: blocks unique aligned sized
// CHECK-NEXT: 8192: blocks unique aligned sized
// CHECK-NEXT: 16384: blocks unique aligned sized
// CHECK-NEXT: 32768: blocks unique aligned sized
// CHECK-NEXT: 65536: blocks unique aligned sized
// CHECK-NEXT: 131072: blocks unique aligned sized
// CHECK-NEXT: 262144: blocks unique aligned sized
// CHECK-NEXT: 524288: blocks unique aligned sized
// CHECK-NEXT: 1048576: blocks unique aligned sized
// CHECK-NEXT: 2097152: blocks unique aligned sized
