// RUN: %clang --target=x86_64-unknown-windows-itanium -### %s 2>&1 | FileCheck %s
// RUN: %clang --target=x86_64-unknown-windows-itanium -### -nostdlib -mwindows %s 2>&1 | FileCheck %s
// RUN: %clang_cl --target=x86_64-unknown-windows-itanium -### %s 2>&1 | FileCheck %s
// CHECK: "-manifest:embed,heap=segment"
// RUN: %clang --target=x86_64-unknown-windows-itanium -### -shared %s 2>&1 | FileCheck %s --check-prefix=DLL
// RUN: %clang_cl --target=x86_64-unknown-windows-itanium -### /LD %s 2>&1 | FileCheck %s --check-prefix=DLL
// DLL-NOT: heap=segment
// DLL: "-dll"
// DLL-NOT: heap=segment

int main(void) { return 0; }
