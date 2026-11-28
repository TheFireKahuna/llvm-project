// RUN: %clang -fbuiltin %s -o %t.exe
// RUN: %run %t.exe
// RUN: llvm-readobj --coff-imports %t.exe | FileCheck %s

// setjmp is the Universal CRT's function that takes the frame clang passes
// with the buffer, under the name clang calls it by: _setjmp, or _setjmpex on
// AArch64. longjmp unwinds to that frame, so a termination handler on the way
// runs. The frame is the builtin's to pass.

// CHECK: Name: ucrtbase.dll
// CHECK: Symbol: {{setjmp|__intrinsic_setjmpex}} (

#include <setjmp.h>

static jmp_buf Buffer;
static volatile int Finally;

__attribute__((noinline)) static void jump(void) {
  __try {
    longjmp(Buffer, 42);
  } __finally {
    Finally = 1;
  }
}

int main(void) {
  volatile int Jumped = 0;
  int Value = setjmp(Buffer);
  if (!Value) {
    Jumped = 1;
    jump();
  }
  return !(Value == 42 && Jumped && Finally);
}
