// RUN: %clang %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
// RUN: %clang -static %s -o %t-static.exe
// RUN: %run %t-static.exe | FileCheck %s

// Every entry object wraps memcpy, rand_s and the other functions that wincrt
// defines again or that another import library offers too, so that the image
// binds wincrt's and the Universal CRT's whichever libraries it names. A
// program's own definition of one still comes first, as it does before any
// library's function: the program's references bind it.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int Copies;

void *memcpy(void *Destination, const void *Source, size_t Size) {
  ++Copies;
  for (size_t I = 0; I != Size; ++I)
    ((char *)Destination)[I] = ((const char *)Source)[I];
  return Destination;
}

errno_t rand_s(unsigned *Value) {
  *Value = 7;
  return 0;
}

int main(void) {
  char Buffer[4];
  void *(*volatile Copy)(void *, const void *, size_t) = memcpy;
  Copy(Buffer, "own", 4);
  unsigned Value = 0;
  errno_t (*volatile Random)(unsigned *) = rand_s;
  Random(&Value);
  printf("memcpy %d %s, rand_s %u\n", Copies != 0, Buffer, Value);
  return 0;
}

// CHECK: memcpy 1 own, rand_s 7
