// RUN: %clang_cc1 -std=c++11 -fms-extensions -emit-llvm %s -o - -triple=x86_64-unknown-windows-itanium | FileCheck --check-prefix=ITANIUM-WIN %s
// RUN: %clang_cc1 -std=c++11 -fms-extensions -emit-llvm %s -o - -triple=aarch64-unknown-windows-itanium | FileCheck --check-prefix=ITANIUM-WIN-ARM64 %s
// RUN: %clang_cc1 -std=c++11 -fms-extensions -emit-llvm %s -o - -triple=x86_64-pc-windows-ntposix | FileCheck --check-prefix=NTPOSIX %s
// RUN: %clang_cc1 -std=c++11 -fms-extensions -emit-llvm %s -o - -triple=aarch64-pc-windows-ntposix | FileCheck --check-prefix=NTPOSIX-ARM64 %s
// RUN: %clang_cc1 -std=c++11 -fms-extensions -emit-llvm %s -o - -triple=x86_64-unknown-windows-itanium -fclang-abi-compat=23 | FileCheck --check-prefix=COMPAT %s

// A non-static member function returns as the Itanium ABI has it: a class
// that is trivial for calls comes back as a free function returns it, and an
// indirect return passes its pointer before 'this'. On Windows Itanium a
// member function that spells __stdcall, __thiscall or ms_abi, as COM methods
// do, or that overrides one and spells none, takes Microsoft's convention,
// which returns every class through a pointer after 'this', in x1 on ARM64.
// The convention is part of the function's type where it returns a class,
// and the default otherwise. NT-POSIX does the same, except that on x86-64,
// whose default is SysV's, the convention is the method's whatever it
// returns. -fclang-abi-compat=23 keeps the Itanium rule.

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

Size callGetSize(Interface *i) { return i->getSize(); }
Desc callGetDesc(Interface *i) { return i->getDesc(); }
Size callPlain(Interface *i) { return i->plain(); }
Size callMsAbi(Interface *i) { return i->msAbi(); }
Size Interface::thisCall() { return {}; }
Size Impl::getSize() { return {1, 2}; }
long Impl::release() { return 0; }

// The convention is a vendor qualifier of the function type where it returns
// a class, and the default otherwise.
template <class T> void take(T) {}
void takeMembers() {
  take(&Interface::getSize);
  take(&Interface::release);
}

// An 8-byte class comes back through a pointer after 'this'.
// ITANIUM-WIN-LABEL: define dso_local i64 @_Z11callGetSizeP9Interface(ptr {{[^,]*}} %i)
// ITANIUM-WIN: call win64cc void %{{.*}}(ptr {{[^,]*}} %{{.*}}, ptr dead_on_unwind writable sret(%struct.Size) align 4 %{{.*}})
// ITANIUM-WIN-LABEL: define dso_local void @_Z11callGetDescP9Interface(ptr dead_on_unwind noalias writable sret(%struct.Desc) align 8 %agg.result, ptr {{[^,]*}} %i)
// ITANIUM-WIN: call win64cc void %{{.*}}(ptr {{[^,]*}} %{{.*}}, ptr dead_on_unwind writable sret(%struct.Desc) align 8 %agg.result)
// A member function that spells no convention keeps the Itanium rule.
// ITANIUM-WIN-LABEL: define dso_local i64 @_Z9callPlainP9Interface(ptr {{[^,]*}} %i)
// ITANIUM-WIN: call i64 %{{.*}}(ptr {{[^,]*}} %{{.*}})
// ITANIUM-WIN-LABEL: define dso_local i64 @_Z9callMsAbiP9Interface(
// ITANIUM-WIN: call win64cc void @_ZN9Interface5msAbiEv(ptr {{[^,]*}} %{{.*}}, ptr dead_on_unwind writable sret(%struct.Size)
// ITANIUM-WIN-LABEL: define dso_local win64cc void @_ZN9Interface8thisCallEv(ptr {{[^,]*}} %this, ptr dead_on_unwind noalias writable sret(%struct.Size)
// An override that spells none takes the convention; one that returns no
// class keeps the default.
// ITANIUM-WIN-LABEL: define dso_local win64cc void @_ZN4Impl7getSizeEv(ptr {{[^,]*}} %this, ptr dead_on_unwind noalias writable sret(%struct.Size)
// ITANIUM-WIN-LABEL: define dso_local noundef i32 @_ZN4Impl7releaseEv(ptr

// ITANIUM-WIN-ARM64-LABEL: define dso_local %struct.Size @_Z11callGetSizeP9Interface(
// ITANIUM-WIN-ARM64: call win64cc void %{{.*}}(ptr {{[^,]*}} %{{.*}}, ptr dead_on_unwind inreg writable sret(%struct.Size) align 4 %{{.*}})
// ITANIUM-WIN-ARM64-LABEL: define dso_local %struct.Size @_Z9callPlainP9Interface(
// ITANIUM-WIN-ARM64: call %struct.Size %{{.*}}(ptr {{[^,]*}} %{{.*}})

// NTPOSIX-LABEL: define dso_local <2 x float> @_Z11callGetSizeP9Interface(
// NTPOSIX: call win64cc void %{{.*}}(ptr {{[^,]*}} %{{.*}}, ptr dead_on_unwind writable sret(%struct.Size) align 4 %{{.*}})
// NTPOSIX-LABEL: define dso_local void @_Z11callGetDescP9Interface(
// NTPOSIX: call win64cc void %{{.*}}(ptr {{[^,]*}} %{{.*}}, ptr dead_on_unwind writable sret(%struct.Desc) align 8 %agg.result)
// NTPOSIX-LABEL: define dso_local <2 x float> @_Z9callPlainP9Interface(
// NTPOSIX: call <2 x float> %{{.*}}(ptr {{[^,]*}} %{{.*}})
// NTPOSIX-LABEL: define dso_local win64cc void @_ZN4Impl7getSizeEv(ptr {{[^,]*}} %this, ptr dead_on_unwind noalias writable sret(%struct.Size)
// NTPOSIX-LABEL: define dso_local win64cc noundef i64 @_ZN4Impl7releaseEv(ptr

// NTPOSIX-ARM64-LABEL: define dso_local %struct.Size @_Z11callGetSizeP9Interface(
// NTPOSIX-ARM64: call win64cc void %{{.*}}(ptr {{[^,]*}} %{{.*}}, ptr dead_on_unwind inreg writable sret(%struct.Size) align 4 %{{.*}})
// NTPOSIX-ARM64-LABEL: define dso_local noundef i64 @_ZN4Impl7releaseEv(ptr

// COMPAT-LABEL: define dso_local i64 @_Z11callGetSizeP9Interface(ptr {{[^,]*}} %i)
// COMPAT: call i64 %{{.*}}(ptr {{[^,]*}} %{{.*}})
// COMPAT-LABEL: define dso_local void @_Z11callGetDescP9Interface(
// COMPAT: call void %{{.*}}(ptr dead_on_unwind writable sret(%struct.Desc) align 8 %agg.result, ptr {{[^,]*}} %{{.*}})

// The instantiations for the member pointer types, emitted last.
// ITANIUM-WIN-DAG: define linkonce_odr {{.*}}@_Z4takeIM9InterfaceU6ms_abiF4SizevEEvT_(
// ITANIUM-WIN-DAG: define linkonce_odr {{.*}}@_Z4takeIM9InterfaceFlvEEvT_(
// COMPAT-DAG: define linkonce_odr {{.*}}@_Z4takeIM9InterfaceF4SizevEEvT_(
