// RUN: %clang_cc1 -triple x86_64-linux-gnu -emit-llvm -o - %s | FileCheck --check-prefix=ELF %s
// RUN: %clang_cc1 -triple x86_64-windows-itanium -emit-llvm -o - %s | FileCheck --check-prefix=COFF %s
// RUN: %clang_cc1 -triple aarch64-windows-ntposix -emit-llvm -o - %s | FileCheck --check-prefix=COFF %s
// RUN: %clang_cc1 -triple x86_64-w64-mingw32 -emit-llvm -o - %s | FileCheck --check-prefix=COFF %s

// The symver attribute gives a definition a versioned name. On ELF it is
// .symver's, on the definition that the directive needs kept. COFF has no
// symbol versions: the name is an external alias of the definition, whatever
// the definition's linkage, and a declaration gets none.

// ELF:      module asm
// ELF-NEXT: ".symver f_v2, f@@V2"
// ELF-NEXT: ".symver f_v2, g@V1"
// ELF-NEXT: ".symver v, v@@V2"
// ELF-NEXT: ".symver f_v1, f@V1"
// ELF:      @llvm.compiler.used = appending global [3 x ptr] [ptr @f_v2, ptr @v, ptr @f_v1]
// ELF-NOT:  alias

// COFF-NOT:  module asm
// COFF-NOT:  llvm.compiler.used
// COFF:      @"f@@V2" = dso_local alias i32 (), ptr @f_v2
// COFF-NEXT: @"g@V1" = dso_local alias i32 (), ptr @f_v2
// COFF-NEXT: @"v@@V2" = hidden alias i32, ptr @v
// COFF-NEXT: @"f@V1" = dso_local alias i32 (), ptr @f_v1
// COFF-NOT:  alias

static __attribute__((symver("f@V1"))) int f_v1(void) { return 1; }
__attribute__((symver("f@@V2"), symver("g@V1"))) int f_v2(void) { return 2; }
__attribute__((symver("v@@V2"), visibility("hidden"))) int v = 2;
__attribute__((symver("declared@V1"))) int declared(void);

int use(void) { return f_v1() + declared(); }
