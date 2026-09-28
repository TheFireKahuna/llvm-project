// RUN: %clang_wincrt -fstack-protector-strong %s -o %t.exe
// RUN: %run %t.exe

// The loader, or failing it the entry point, replaces the default cookie
// with a 48-bit value before main, and protected frames check against it.

#include <stdint.h>
#include <string.h>

extern uintptr_t __security_cookie;

__attribute__((noinline)) static int protectedFrame(const char *Text) {
  char Buffer[64];
  strcpy(Buffer, Text);
  return (int)strlen(Buffer);
}

int main(void) {
  if (__security_cookie == 0x2B992DDFA232 || __security_cookie >> 48)
    return 1;
  return protectedFrame("protected") != 9;
}
