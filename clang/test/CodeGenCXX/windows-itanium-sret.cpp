// RUN: %clang_cc1 -std=c++11 -emit-llvm %s -o - -triple=x86_64-unknown-windows-itanium | FileCheck --check-prefix=ITANIUM-WIN %s
// RUN: %clang_cc1 -std=c++11 -emit-llvm %s -o - -triple=aarch64-unknown-windows-itanium | FileCheck --check-prefix=ITANIUM-WIN-ARM64 %s
// RUN: %clang_cc1 -std=c++11 -emit-llvm %s -o - -triple=x86_64-pc-windows-ntposix | FileCheck --check-prefix=NTPOSIX %s
// RUN: %clang_cc1 -std=c++11 -emit-llvm %s -o - -triple=x86_64-w64-windows-gnu | FileCheck --check-prefix=MINGW %s
// RUN: %clang_cc1 -std=c++11 -emit-llvm %s -o - -triple=x86_64-unknown-windows-itanium -fclang-abi-compat=23 | FileCheck --check-prefix=MINGW %s

// Windows Itanium returns a class from a non-static member function through
// a pointer that follows 'this', as Microsoft's calling convention does, so
// that a COM interface implemented by MSVC-built code is called correctly
// through its vtable. Which classes a free or static member function returns
// in registers stays the Itanium rule. NT-POSIX and MinGW keep the pointer
// first, as does -fclang-abi-compat=23.

struct Size { float w, h; };
struct Desc { long long a, b, c; };
struct WithCtor { WithCtor(); int x; };
struct NonTrivial { NonTrivial(const NonTrivial &); int x; };

struct Interface {
  virtual Size getSize() = 0;
  virtual Desc getDesc() = 0;
  static Size staticSize();
  NonTrivial getNonTrivial();
};

Size callGetSize(Interface *i) { return i->getSize(); }
Desc callGetDesc(Interface *i) { return i->getDesc(); }
Size callStaticSize() { return Interface::staticSize(); }
NonTrivial callGetNonTrivial(Interface *i) { return i->getNonTrivial(); }
WithCtor makeWithCtor();
int useWithCtor() { return makeWithCtor().x; }

// An 8-byte class that a free function returns in RAX comes back through a
// pointer after 'this' from an instance method.
// ITANIUM-WIN-LABEL: define dso_local i64 @_Z11callGetSizeP9Interface(ptr {{[^,]*}} %i)
// ITANIUM-WIN: call void %{{.*}}(ptr {{[^,]*}} %{{.*}}, ptr dead_on_unwind writable sret(%struct.Size) align 4 %{{.*}})
// ITANIUM-WIN-LABEL: define dso_local void @_Z11callGetDescP9Interface(ptr dead_on_unwind noalias writable sret(%struct.Desc) align 8 %agg.result, ptr {{[^,]*}} %i)
// ITANIUM-WIN: call void %{{.*}}(ptr {{[^,]*}} %{{.*}}, ptr dead_on_unwind writable sret(%struct.Desc) align 8 %agg.result)
// A static member function keeps the rules of a free function.
// ITANIUM-WIN-LABEL: define dso_local i64 @_Z14callStaticSizev()
// ITANIUM-WIN: call i64 @_ZN9Interface10staticSizeEv()
// A class that cannot be passed in registers moves after 'this' too.
// ITANIUM-WIN-LABEL: define dso_local void @_Z17callGetNonTrivialP9Interface(ptr dead_on_unwind noalias writable sret(%struct.NonTrivial) align 4 %agg.result, ptr {{[^,]*}} %i)
// ITANIUM-WIN: call void @_ZN9Interface13getNonTrivialEv(ptr {{[^,]*}} %{{.*}}, ptr dead_on_unwind writable sret(%struct.NonTrivial) align 4 %agg.result)
// A class with a user-provided constructor is still returned in a register
// from a free function, as the Itanium ABI says; MSVC would use a pointer.
// ITANIUM-WIN-LABEL: define dso_local noundef i32 @_Z11useWithCtorv()
// ITANIUM-WIN: call i32 @_Z12makeWithCtorv()

// On ARM64 the pointer after 'this' is passed in x1, which 'inreg' selects.
// ITANIUM-WIN-ARM64-LABEL: define dso_local %struct.Size @_Z11callGetSizeP9Interface(
// ITANIUM-WIN-ARM64: call void %{{.*}}(ptr {{[^,]*}} %{{.*}}, ptr dead_on_unwind inreg writable sret(%struct.Size) align 4 %{{.*}})
// ITANIUM-WIN-ARM64-LABEL: define dso_local %struct.Size @_Z14callStaticSizev()
// ITANIUM-WIN-ARM64: call %struct.Size @_ZN9Interface10staticSizeEv()
// ITANIUM-WIN-ARM64-LABEL: define dso_local void @_Z17callGetNonTrivialP9Interface(
// ITANIUM-WIN-ARM64: call void @_ZN9Interface13getNonTrivialEv(ptr {{[^,]*}} %{{.*}}, ptr dead_on_unwind inreg writable sret(%struct.NonTrivial) align 4 %agg.result)

// MINGW-LABEL: define dso_local i64 @_Z11callGetSizeP9Interface(ptr {{[^,]*}} %i)
// MINGW: call i64 %{{.*}}(ptr {{[^,]*}} %{{.*}})
// MINGW-LABEL: define dso_local void @_Z11callGetDescP9Interface(ptr dead_on_unwind noalias writable sret(%struct.Desc) align 8 %agg.result, ptr {{[^,]*}} %i)
// MINGW: call void %{{.*}}(ptr dead_on_unwind writable sret(%struct.Desc) align 8 %agg.result, ptr {{[^,]*}} %{{.*}})
// MINGW-LABEL: define dso_local void @_Z17callGetNonTrivialP9Interface(
// MINGW: call void @_ZN9Interface13getNonTrivialEv(ptr dead_on_unwind writable sret(%struct.NonTrivial) align 4 %agg.result, ptr {{[^,]*}} %{{.*}})

// NT-POSIX keeps the pointer first.
// NTPOSIX-LABEL: define dso_local void @_Z11callGetDescP9Interface(
// NTPOSIX: call void %{{.*}}(ptr dead_on_unwind writable sret(%struct.Desc) align 8 %agg.result, ptr {{[^,]*}} %{{.*}})
// NTPOSIX-LABEL: define dso_local void @_Z17callGetNonTrivialP9Interface(
// NTPOSIX: call void @_ZN9Interface13getNonTrivialEv(ptr dead_on_unwind writable sret(%struct.NonTrivial) align 4 %agg.result, ptr {{[^,]*}} %{{.*}})
