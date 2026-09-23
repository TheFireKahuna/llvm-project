// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fdeclspec -fvisibility=hidden -emit-llvm -o - %s | FileCheck %s --implicit-check-not=@_ZTV14InlineImported
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fdeclspec -fno-dllexport-inlines -fvisibility=hidden -emit-llvm -o - %s | FileCheck %s --implicit-check-not=@_ZTV14InlineImported
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -fdeclspec -fno-dllexport-inlines -fvisibility=hidden -emit-llvm -o - %s | FileCheck %s --implicit-check-not=@_ZTV14InlineImported
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fdeclspec -fno-dllexport-inlines -fno-rtti -emit-llvm -o - %s | FileCheck %s --check-prefix=NO-RTTI --implicit-check-not=@_ZTI14InlineExported --implicit-check-not=@_ZTV14InlineImported

class __declspec(dllexport) Exported {
public:
  virtual ~Exported();
};
Exported::~Exported() {}

// An explicit published owner exports both canonical identities, including
// under -fvisibility=hidden.
// CHECK-DAG: @_ZTS8Exported = dso_local dllexport constant
// CHECK-DAG: @_ZTI8Exported = dso_local dllexport constant

// Export class metadata even when no member needs an out-of-line definition.
// This also applies when inline members are excluded from DLL export.
class __declspec(dllexport) InlineExported {
public:
  virtual ~InlineExported() = default;
};

// CHECK-DAG: @_ZTV14InlineExported = weak_odr {{.*}}dllexport
// CHECK-DAG: @_ZTS14InlineExported = weak_odr dllexport constant
// CHECK-DAG: @_ZTI14InlineExported = weak_odr dllexport constant
// NO-RTTI: @_ZTV14InlineExported = weak_odr {{.*}}dllexport

class __declspec(dllimport) InlineImported {
public:
  virtual ~InlineImported() = default;
};
