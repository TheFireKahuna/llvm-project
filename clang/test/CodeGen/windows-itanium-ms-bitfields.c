// RUN: %clang_cc1 -emit-llvm-only -triple x86_64-unknown-windows-itanium %s
// RUN: %clang_cc1 -emit-llvm-only -triple aarch64-unknown-windows-itanium %s
// RUN: %clang_cc1 -emit-llvm-only -triple x86_64-unknown-windows-itanium -fms-layout-compatibility=itanium -DGCC_LAYOUT %s
// RUN: %clang_cc1 -emit-llvm-only -triple x86_64-unknown-windows-itanium -fclang-abi-compat=23 -DGCC_LAYOUT %s
// RUN: %clang_cc1 -emit-llvm-only -triple x86_64-pc-windows-msvc %s
// RUN: %clang_cc1 -emit-llvm-only -triple x86_64-pc-windows-ntposix -DGCC_LAYOUT %s
// RUN: %clang_cc1 -emit-llvm-only -triple x86_64-unknown-linux-gnu -DGCC_LAYOUT %s

// Windows Itanium lays out bit-fields as MSVC and MinGW do, so a struct from a
// Microsoft-built C library has one layout on both sides of a call.
// -mno-ms-bitfields, which the driver passes as
// -fms-layout-compatibility=itanium, restores GCC's layout, as does
// -fclang-abi-compat=23. NT-POSIX, which uses no Microsoft-built C runtime,
// keeps GCC's layout.

struct mixed_units {
  char a : 4;
  int b : 4;
} t1;

struct different_widths {
  unsigned char a : 1;
  unsigned short b : 1;
} t2;

struct wide_field {
  unsigned a : 3;
  unsigned long long b : 40;
} t3;

#ifdef GCC_LAYOUT
_Static_assert(sizeof(t1) == 4, "");
_Static_assert(sizeof(t2) == 2, "");
_Static_assert(sizeof(t3) == 8, "");
#else
_Static_assert(sizeof(t1) == 8, "");
_Static_assert(sizeof(t2) == 4, "");
_Static_assert(sizeof(t3) == 16, "");
#endif
