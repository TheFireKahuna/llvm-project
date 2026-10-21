// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o - %s \
// RUN:   | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -emit-llvm -o - %s \
// RUN:   | FileCheck %s

// On Windows Itanium and NT-POSIX a type_info object that reaches a class
// incomplete in the translation unit is a weak alias of its own name, whose
// default is a placeholder under a name of its own with the ABI's incomplete
// flags, kept once in an image, so that the link binds it to the complete
// class's type_info object wherever an input defines that. A pointer's name
// holds no layout, so the placeholder shares it; the placeholder for the class
// has a name of its own, since the complete class's is followed by its layout.

namespace std {
class type_info;
} // namespace std

struct Foo;

// CHECK-DAG: @"__llvm_incomplete$_ZTIPP3Foo" = linkonce_odr constant { ptr, ptr, i32, ptr } { ptr {{.*}}, ptr inttoptr (i64 add (i64 ptrtoint (ptr @_ZTSPP3Foo to i64), i64 {{.*}}) to ptr), i32 8, ptr @_ZTIP3Foo }, comdat
// CHECK-DAG: @"__llvm_incomplete$_ZTIP3Foo" = linkonce_odr constant { ptr, ptr, i32, ptr } { ptr {{.*}}, ptr inttoptr (i64 add (i64 ptrtoint (ptr @_ZTSP3Foo to i64), i64 {{.*}}) to ptr), i32 8, ptr @_ZTI3Foo }, comdat
// CHECK-DAG: @"__llvm_incomplete$_ZTI3Foo" = linkonce_odr constant { ptr, ptr } { ptr {{.*}}, ptr inttoptr (i64 add (i64 ptrtoint (ptr @"__llvm_incomplete$_ZTS3Foo" to i64), i64 -9221964661971222528) to ptr) }, comdat
// CHECK-DAG: @"__llvm_incomplete$_ZTS3Foo" = linkonce_odr constant [5 x i8] c"3Foo\00", comdat
// CHECK-DAG: @_ZTSPP3Foo = linkonce_odr constant [7 x i8] c"PP3Foo\00", comdat
// CHECK-DAG: @"__llvm_incomplete$_ZTIM3Fooi" = linkonce_odr constant { ptr, ptr, i32, ptr, ptr } { ptr {{.*}}, ptr {{.*}}, i32 16, ptr @_ZTIi, ptr @_ZTI3Foo }, comdat
// CHECK-DAG: @_ZTI3Foo = weak alias { ptr, ptr }, ptr @"__llvm_incomplete$_ZTI3Foo"
// CHECK-DAG: @_ZTIP3Foo = weak alias { ptr, ptr, i32, ptr }, ptr @"__llvm_incomplete$_ZTIP3Foo"
// CHECK-DAG: @_ZTIPP3Foo = weak alias { ptr, ptr, i32, ptr }, ptr @"__llvm_incomplete$_ZTIPP3Foo"
// CHECK-DAG: @_ZTIM3Fooi = weak alias { ptr, ptr, i32, ptr, ptr }, ptr @"__llvm_incomplete$_ZTIM3Fooi"

const std::type_info &pp() { return typeid(Foo **); }
const std::type_info &mp() { return typeid(int Foo::*); }
