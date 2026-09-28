// RUN: %clang_cc1 -triple x86_64-windows-gnu -emit-llvm -std=c++11 -o - %s | FileCheck --check-prefix=GNU %s
// RUN: %clang_cc1 -triple x86_64-pc-cygwin   -emit-llvm -std=c++11 -o - %s | FileCheck --check-prefix=GNU %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fdeclspec -fno-dllexport-inlines -emit-llvm -std=c++11 -o - %s | FileCheck --check-prefix=ITANIUM %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -fdeclspec -fno-dllexport-inlines -emit-llvm -std=c++11 -o - %s | FileCheck --check-prefix=ITANIUM %s

// The vtable of an exported class is exported on every Itanium COFF target,
// even when no exported member refers to it: here the members are inline, and
// MinGW does not export them, nor does -fno-dllexport-inlines.

class __declspec(dllexport) QAbstractLayoutStyleInfo {
public:
  QAbstractLayoutStyleInfo() : m_isWindow(false) {}
  virtual ~QAbstractLayoutStyleInfo() {}

  virtual bool hasChangedCore() const { return false; }

  virtual void invalidate() {}

  virtual double windowMargin(bool orientation) const = 0;

  bool isWindow() const { return m_isWindow; }

protected:
  bool m_isWindow;
};

// GNU-DAG: @_ZTV24QAbstractLayoutStyleInfo = weak_odr dso_local dllexport
// GNU-DAG: @_ZTS24QAbstractLayoutStyleInfo = linkonce_odr
// GNU-DAG: @_ZTI24QAbstractLayoutStyleInfo = linkonce_odr

// ITANIUM: @_ZTV24QAbstractLayoutStyleInfo = weak_odr dso_local dllexport
