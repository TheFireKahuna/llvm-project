// RUN: %clang -fstack-protector-strong %s -Wl,-guard:no -o %t.exe
// RUN: %run %t.exe
// RUN: llvm-readobj --coff-load-config --sections %t.exe | FileCheck %s

// The load configuration names the security cookie, so the loader, or failing
// it the entry point, replaces the default value with a 48-bit one before
// main, and protected frames check against it. The fields of absent features,
// here the guards', are zero.

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

// CHECK:      Name: .data
// CHECK:      VirtualAddress: 0x[[#%X,DATA:]]
// CHECK:      LoadConfig [
// CHECK-NEXT:   Size: 0x148
// CHECK:        SecurityCookie: 0x14000[[#%X,DATA]]
// CHECK:        GuardCFFunctionTable: 0x0
// CHECK-NEXT:   GuardCFFunctionCount: 0
// CHECK-NEXT:   GuardFlags [
// CHECK:        GuardAddressTakenIatEntryTable: 0x0
// CHECK:        EnclaveConfigurationPointer: 0x0
// CHECK-NEXT:   VolatileMetadataPointer: 0x0
// CHECK-NEXT:   GuardEHContinuationTable: 0x0
// CHECK-NEXT:   GuardEHContinuationCount: 0
