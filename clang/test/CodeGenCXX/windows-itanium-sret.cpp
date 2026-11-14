// RUN: %clang_cc1 -std=c++11 -fms-extensions -emit-llvm %s -o - -triple=x86_64-unknown-windows-itanium | FileCheck --check-prefixes=CHECK,X64 %s
// RUN: %clang_cc1 -std=c++11 -fms-extensions -emit-llvm %s -o - -triple=aarch64-unknown-windows-itanium | FileCheck --check-prefixes=CHECK,ARM64 %s
// RUN: %clang_cc1 -std=c++11 -fms-extensions -emit-llvm %s -o - -triple=x86_64-unknown-windows-itanium -fclang-abi-compat=23 | FileCheck --check-prefix=COMPAT %s
// RUN: %clang_cc1 -std=c++11 -emit-llvm %s -o - -triple=x86_64-pc-windows-ntposix -D__stdcall= -D__thiscall= | FileCheck --check-prefix=NTPOSIX %s

// A non-static member function returns as the Itanium C++ ABI has it: a class
// that is trivial for calls comes back as a free function returns it, and an
// indirect return passes its pointer before 'this'. On Windows Itanium a
// member function that spells __stdcall, __thiscall or ms_abi, as a COM method
// does, or that overrides one and spells none, takes Microsoft's convention,
// which returns every class through a pointer after 'this', in x1 on ARM64.
// The convention is part of the function's type where it returns a class and
// the default otherwise. A free function returns as Itanium has it whatever
// it spells.

struct Size { float w, h; };
struct Desc { long long a, b, c; };

struct Interface {
  virtual Size __stdcall getSize() = 0;
  virtual Desc __stdcall getDesc() = 0;
  virtual long __stdcall release() = 0;
  virtual Size plain() = 0;
  Size __attribute__((ms_abi)) msAbi();
  Size __thiscall thisCall();
};

struct Impl : Interface {
  Size getSize() override;
  Desc getDesc() override;
  long release() override;
  Size plain() override;
};

Size __stdcall freeFunction();

Size callGetSize(Interface *i) { return i->getSize(); }
Desc callGetDesc(Interface *i) { return i->getDesc(); }
Size callPlain(Interface *i) { return i->plain(); }
Size callMsAbi(Interface *i) { return i->msAbi(); }
Size callFree() { return freeFunction(); }
Size Interface::thisCall() { return {}; }
Size Impl::getSize() { return {1, 2}; }
long Impl::release() { return 0; }

template <class T> void take(T) {}
void takeMembers() {
  take(&Interface::getSize);
  take(&Interface::release);
}

// An 8-byte class comes back through a pointer after 'this'.
// CHECK-LABEL: define dso_local {{.*}} @_Z11callGetSizeP9Interface(ptr {{[^,]*}} %i)
// X64:   call win64cc void %{{.*}}(ptr {{[^,]*}} %{{.*}}, ptr dead_on_unwind writable sret(%struct.Size) align 4 %{{.*}})
// ARM64: call win64cc void %{{.*}}(ptr {{[^,]*}} %{{.*}}, ptr dead_on_unwind inreg writable sret(%struct.Size) align 4 %{{.*}})
// CHECK-LABEL: define dso_local void @_Z11callGetDescP9Interface(ptr dead_on_unwind noalias writable sret(%struct.Desc) align 8 %agg.result, ptr {{[^,]*}} %i)
// X64:   call win64cc void %{{.*}}(ptr {{[^,]*}} %{{.*}}, ptr dead_on_unwind writable sret(%struct.Desc) align 8 %agg.result)
// ARM64: call win64cc void %{{.*}}(ptr {{[^,]*}} %{{.*}}, ptr dead_on_unwind inreg writable sret(%struct.Desc) align 8 %agg.result)

// A member function that spells no convention keeps the Itanium rule.
// CHECK-LABEL: define dso_local {{.*}} @_Z9callPlainP9Interface(
// X64:   call i64 %{{.*}}(ptr {{[^,]*}} %{{.*}})
// ARM64: call %struct.Size %{{.*}}(ptr {{[^,]*}} %{{.*}})

// CHECK-LABEL: define dso_local {{.*}} @_Z9callMsAbiP9Interface(
// CHECK: call win64cc void @_ZN9Interface5msAbiEv(ptr {{[^,]*}} %{{.*}}, ptr dead_on_unwind {{(inreg )?}}writable sret(%struct.Size)

// A free function keeps the Itanium rule under the convention.
// CHECK-LABEL: define dso_local {{.*}} @_Z8callFreev(
// X64:   call win64cc i64 @_Z12freeFunctionv()
// ARM64: call win64cc %struct.Size @_Z12freeFunctionv()

// CHECK-LABEL: define dso_local win64cc void @_ZN9Interface8thisCallEv(ptr {{[^,]*}} %this, ptr dead_on_unwind {{(inreg )?}}noalias writable sret(%struct.Size)

// An override that spells none takes the convention; one that returns no
// class keeps the default.
// CHECK-LABEL: define dso_local win64cc void @_ZN4Impl7getSizeEv(ptr {{[^,]*}} %this, ptr dead_on_unwind {{(inreg )?}}noalias writable sret(%struct.Size)
// CHECK-LABEL: define dso_local noundef i32 @_ZN4Impl7releaseEv(ptr

// The convention is a vendor qualifier of the function type where it returns
// a class.
// CHECK-DAG: define linkonce_odr {{.*}}@_Z4takeIM9InterfaceU6ms_abiF4SizevEEvT_(
// CHECK-DAG: define linkonce_odr {{.*}}@_Z4takeIM9InterfaceFlvEEvT_(

// -fclang-abi-compat=23 ignores the conventions.
// COMPAT-LABEL: define dso_local i64 @_Z11callGetSizeP9Interface(ptr {{[^,]*}} %i)
// COMPAT: call i64 %{{.*}}(ptr {{[^,]*}} %{{.*}})
// COMPAT-LABEL: define dso_local void @_Z11callGetDescP9Interface(
// COMPAT: call void %{{.*}}(ptr dead_on_unwind writable sret(%struct.Desc) align 8 %agg.result, ptr {{[^,]*}} %{{.*}})
// COMPAT-DAG: define linkonce_odr {{.*}}@_Z4takeIM9InterfaceF4SizevEEvT_(

// NT-POSIX keeps the Itanium rule for a member function that spells ms_abi.
// NTPOSIX-LABEL: define dso_local {{.*}} @_Z9callMsAbiP9Interface(
// NTPOSIX: call win64cc i64 @_ZN9Interface5msAbiEv(ptr {{[^,]*}} %{{.*}})
