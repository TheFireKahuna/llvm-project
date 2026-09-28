// RUN: %clang_wincrt %s %aligned_alloc -o %t.exe
// RUN: %run %t.exe | FileCheck %s

// An alignment up to 4 KiB with a small size is served by the
// low-fragmentation heap's class for the next power of two, whose block keeps
// that size. The heap takes on a size only after some requests for it, so
// the first miss in a size warms it up, and every request in that size then
// gets a class block, even one that only aligned_alloc uses. A request the
// class did not serve would come from 4 KiB pages, shrunk to the request.

#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define COUNT 400

static void check(size_t Alignment, size_t Size, size_t ClassSize) {
  static void *Blocks[COUNT];
  int Aligned = 0, Class = 0;
  for (int I = 0; I < COUNT; ++I) {
    Blocks[I] = aligned_alloc(Alignment, Size);
    Aligned += Blocks[I] && (uintptr_t)Blocks[I] % Alignment == 0;
    Class += Blocks[I] && _msize(Blocks[I]) == ClassSize;
  }
  for (int I = 0; I < COUNT; ++I)
    free(Blocks[I]);
  printf("%zu, %zu: %d aligned, %d from the class\n", Alignment, Size, Aligned,
         Class);
}

int main(void) {
  check(64, 40, 64);
  check(256, 200, 256);
  check(1024, 1000, 1024);
  check(2048, 3000, 4096);
  check(4096, 5000, 8192);
  return 0;
}

// CHECK:      64, 40: 400 aligned, 400 from the class
// CHECK-NEXT: 256, 200: 400 aligned, 400 from the class
// CHECK-NEXT: 1024, 1000: 400 aligned, 400 from the class
// CHECK-NEXT: 2048, 3000: 400 aligned, 400 from the class
// CHECK-NEXT: 4096, 5000: 400 aligned, 400 from the class
