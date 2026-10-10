// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -fms-extensions \
// RUN:     -fms-compatibility -fms-compatibility-version=19.33 -ffreestanding \
// RUN:     -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -fms-compatibility -fms-compatibility-version=19.33 -ffreestanding \
// RUN:     -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -fms-compatibility -fms-compatibility-version=19.33 -ffreestanding \
// RUN:     -emit-llvm -o - -x c++ %s | FileCheck %s

// The functions intrin.h defines are only ever inlined: no unit emits a
// definition, even after a non-inline declaration such as winnt.h's, so that
// units including the header link together.

typedef __SIZE_TYPE__ size_t;

#ifdef __cplusplus
extern "C" {
#endif
void __movsb(unsigned char *, unsigned char const *, size_t);
unsigned char __inbyte(unsigned short);
void __halt(void);
#ifdef __cplusplus
}
#endif

#include <intrin.h>

// CHECK-NOT: {{^(define|declare) .*@__}}
// CHECK-LABEL: define {{.*}}test
// CHECK: asm sideeffect "rep movsb"
// CHECK: asm sideeffect "rep stosq"
// CHECK: asm sideeffect "inb
// CHECK: asm sideeffect "outl
// CHECK: asm sideeffect "hlt"
// CHECK: asm "rdmsr"
// CHECK: asm sideeffect "mov $($0, %cr3
// CHECK-NOT: {{^(define|declare) .*@__}}
void test(unsigned char *d, const unsigned char *s, unsigned __int64 *q,
          size_t n) {
  __movsb(d, s, n);
  __stosq(q, 0, n);
  __inbyte(0x80);
  __outdword(0x80, 0);
  __halt();
  __readmsr(0);
  __writecr3(0);
}
