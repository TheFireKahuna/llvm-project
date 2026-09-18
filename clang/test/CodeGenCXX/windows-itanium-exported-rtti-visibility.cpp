// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fdeclspec -fvisibility=hidden -emit-llvm -o - %s | FileCheck %s --implicit-check-not=@_ZTV14InlineImported
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fdeclspec -fno-dllexport-inlines -fvisibility=hidden -emit-llvm -o - %s | FileCheck %s --implicit-check-not=@_ZTV14InlineImported
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -fdeclspec -fno-dllexport-inlines -fvisibility=hidden -emit-llvm -o - %s | FileCheck %s --implicit-check-not=@_ZTV14InlineImported
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fdeclspec -fno-dllexport-inlines -fno-rtti -emit-llvm -o - %s | FileCheck %s --check-prefix=NO-RTTI --implicit-check-not=@_ZTI14InlineExported --implicit-check-not=@_ZTV14InlineImported

class __declspec(dllexport) Exported {
public:
  virtual ~Exported();
};
Exported::~Exported() {}

// RTTI is never exported: every image carries its own constant copy, and
// -fvisibility=hidden simply applies to it.
// CHECK-DAG: @_ZTS8Exported = linkonce_odr hidden constant
// CHECK-DAG: @_ZTI8Exported = linkonce_odr hidden constant

// Export class metadata even when no member needs an out-of-line definition.
// This also applies when inline members are excluded from DLL export.
class __declspec(dllexport) InlineExported {
public:
  virtual ~InlineExported() = default;
};

// CHECK-DAG: @_ZTV14InlineExported = weak_odr {{.*}}dllexport
// CHECK-DAG: @_ZTS14InlineExported = linkonce_odr hidden constant
// CHECK-DAG: @_ZTI14InlineExported = linkonce_odr hidden constant
// NO-RTTI: @_ZTV14InlineExported = weak_odr {{.*}}dllexport

class __declspec(dllimport) InlineImported {
public:
  virtual ~InlineImported() = default;
};
