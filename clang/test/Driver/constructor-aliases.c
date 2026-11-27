// RUN: %clang -### --target=x86_64-linux-gnu -c %s 2>&1 | FileCheck %s
// RUN: %clang -### --target=x86_64-linux-gnu -mno-constructor-aliases \
// RUN:   -mconstructor-aliases -c %s 2>&1 | FileCheck %s
// RUN: %clang -### --target=x86_64-linux-gnu -mno-constructor-aliases -c %s \
// RUN:   2>&1 | FileCheck %s --check-prefix=NO
// RUN: %clang -### --target=x86_64-unknown-windows-itanium \
// RUN:   -mno-constructor-aliases -c %s 2>&1 | FileCheck %s --check-prefix=NO

// CHECK: "-mconstructor-aliases"
// NO-NOT: warning:
// NO-NOT: "-mconstructor-aliases"
