// DEFINE: %{cc} = %clang_cc1 -fms-extensions -std=c++20 -emit-llvm -o - %s
// RUN: %{cc} -triple x86_64-unknown-windows-itanium | FileCheck %s
// RUN: %{cc} -triple aarch64-pc-windows-ntposix | FileCheck %s
// RUN: %{cc} -triple x86_64-unknown-windows-itanium \
// RUN:   -fexperimental-new-constant-interpreter | FileCheck %s
// RUN: %{cc} -triple x86_64-w64-windows-gnu \
// RUN:   | FileCheck %s --check-prefix=RUNTIME

// On Windows Itanium and NT-POSIX static data that holds the address of a
// dllimport function or variable is initialized by a constant, so a const
// object stays read-only and nothing runs at startup. On the other Windows
// targets a startup function reads the address from the import table.

struct Agg {
  int Field;
  int Array[4];
};
__declspec(dllimport) extern int Var;
__declspec(dllimport) extern Agg Object;
__declspec(dllimport) void function();
struct Class {
  __declspec(dllimport) void method();
};

// CHECK: @VarAddress = dso_local global ptr @Var
// CHECK: @Interior = dso_local global ptr getelementptr {{.*}}@Object, i64 12)
// CHECK: @MemberPointer = dso_local global { i64, i64 } { i64 ptrtoint (ptr @_ZN5Class6methodEv to i64), i64 0 }
// CHECK: @_ZL5Table = internal constant [2 x ptr] [ptr @_Z8functionv, ptr @Var]
// CHECK-NOT: @llvm.global_ctors
// RUNTIME: @VarAddress = dso_local global ptr null
// RUNTIME: @_ZL5Table = internal global [2 x ptr] zeroinitializer
// RUNTIME: @llvm.global_ctors
int *VarAddress = &Var;
int *Interior = &Object.Array[2];
void (Class::*MemberPointer)() = &Class::method;
static void *const Table[] = {(void *)function, &Var};
void *const *table() { return Table; }
