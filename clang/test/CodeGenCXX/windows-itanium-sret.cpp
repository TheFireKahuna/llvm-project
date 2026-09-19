// Windows Itanium places a hidden return pointer as Microsoft's calling
// convention does: a class returned from a non-static member function goes
// through a pointer that follows 'this', so a COM interface implemented by
// MSVC-built code is called correctly through its vtable. Which classes a
// free function returns in registers stays the Itanium rule, and MinGW keeps
// GCC's placement throughout.
//
// RUN: %clang_cc1 -std=c++11 -emit-llvm %s -o - -triple=x86_64-unknown-windows-itanium | FileCheck --check-prefix=ITANIUM-WIN %s
// RUN: %clang_cc1 -std=c++11 -emit-llvm %s -o - -triple=aarch64-unknown-windows-itanium | FileCheck --check-prefix=ITANIUM-WIN-ARM64 %s
// RUN: %clang_cc1 -std=c++11 -emit-llvm %s -o - -triple=x86_64-w64-windows-gnu | FileCheck --check-prefix=MINGW %s
// RUN: %clang_cc1 -std=c++11 -emit-llvm %s -o - -triple=x86_64-pc-windows-msvc -fno-rtti | FileCheck --check-prefix=MSVC %s

struct Size { float w, h; };
struct Desc { long long a, b, c; };
struct WithCtor { WithCtor(); int x; };

struct Interface {
  virtual Size getSize() = 0;
  virtual Desc getDesc() = 0;
  static Size staticSize();
};

Size callGetSize(Interface *i) { return i->getSize(); }
Desc callGetDesc(Interface *i) { return i->getDesc(); }
Size callStaticSize() { return Interface::staticSize(); }
WithCtor makeWithCtor();
int useWithCtor() { return makeWithCtor().x; }

// An 8-byte class comes back through a pointer after 'this' from an instance
// method; a static member function keeps the C rules.
// ITANIUM-WIN-LABEL: define dso_local i64 @_Z11callGetSizeP9Interface(ptr {{[^,]*}} %i)
// ITANIUM-WIN: call void %{{.*}}(ptr {{[^,]*}} %{{.*}}, ptr dead_on_unwind writable sret(%struct.Size) align 4 %{{.*}})
// ITANIUM-WIN-LABEL: define dso_local void @_Z11callGetDescP9Interface(ptr dead_on_unwind noalias writable sret(%struct.Desc) align 8 %agg.result, ptr {{[^,]*}} %i)
// ITANIUM-WIN: call void %{{.*}}(ptr {{[^,]*}} %{{.*}}, ptr dead_on_unwind writable sret(%struct.Desc) align 8 %agg.result)
// ITANIUM-WIN-LABEL: define dso_local i64 @_Z14callStaticSizev()
// ITANIUM-WIN: call i64 @_ZN9Interface10staticSizeEv()
// A class with a user-provided constructor is still returned in a register
// from a free function, as the Itanium ABI says; MSVC would use a pointer.
// ITANIUM-WIN: call i32 @_Z12makeWithCtorv()

// ITANIUM-WIN-ARM64: call void %{{.*}}(ptr {{[^,]*}} %{{.*}}, ptr dead_on_unwind inreg writable sret(%struct.Size) align 4 %{{.*}})
// ITANIUM-WIN-ARM64: call %struct.Size @_ZN9Interface10staticSizeEv()

// MINGW-LABEL: define dso_local i64 @_Z11callGetSizeP9Interface(ptr {{[^,]*}} %i)
// MINGW: call i64 %{{.*}}(ptr {{[^,]*}} %{{.*}})
// MINGW-LABEL: define dso_local void @_Z11callGetDescP9Interface(ptr dead_on_unwind noalias writable sret(%struct.Desc) align 8 %agg.result, ptr {{[^,]*}} %i)
// MINGW: call void %{{.*}}(ptr dead_on_unwind writable sret(%struct.Desc) align 8 %agg.result, ptr {{[^,]*}} %{{.*}})

// MSVC: call void %{{.*}}(ptr {{[^,]*}} %{{.*}}, ptr dead_on_unwind writable sret(%struct.Size) align 4 %{{.*}})
// MSVC: call void %{{.*}}(ptr {{[^,]*}} %{{.*}}, ptr dead_on_unwind writable sret(%struct.Desc) align 8 %agg.result)
// MSVC: call void @"?makeWithCtor@@YA?AUWithCtor@@XZ"(ptr dead_on_unwind writable sret(%struct.WithCtor) align 4 %{{.*}})
