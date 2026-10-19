// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o - %s \
// RUN:   | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -emit-llvm -o - %s \
// RUN:   | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o - %s \
// RUN:   -DNO_TAGS | FileCheck --check-prefix=NOTAG %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o - %s \
// RUN:   -fno-rtti | FileCheck --check-prefix=NORTTI %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o - %s \
// RUN:   | FileCheck --check-prefix=FACT %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm -o - %s \
// RUN:   | FileCheck --check-prefix=ELF %s

// On Windows Itanium and NT-POSIX every vtable's primary address point is
// placed: at offset 16 of a 64-byte line, or, for a class the exact
// dynamic_cast tags, at the tag's residue modulo a page. The address point is
// 16 + 8k bytes into the vtable, k counting the vcall and vbase offsets of a
// class with virtual bases, so the vtable is aligned to 64 only when they fill
// whole lines. Every placed vtable carries a pin, which is required only for a
// tagged class.

struct V { virtual void v(); };
struct VI { virtual void v() {} };

// k = 0: align 64, with a pin that says as much. Without RTTI, vtables of
// different classes can be identical, and the pins keep the linker from
// folding them.
// CHECK-DAG: @_ZTV5Plain = {{.*}} align 64, !pin ![[K0:[0-9]+]], !vtable_hierarchy {{![0-9]+$}}
// NORTTI-DAG: @_ZTV5Plain = {{.*}} align 64, !pin
// NORTTI-DAG: @_ZTV6VBased = {{.*}} align 8, !pin
// CHECK-DAG: ![[K0]] = !{i64 16, i64 6, i64 16, i64 0}
struct Plain { virtual void f(); };
void Plain::f() {}

// k = 2, one vcall and one vbase offset: the address point is 32 bytes in.
// CHECK-DAG: @_ZTV6VBased = {{.*}} align 8, !pin ![[VB:[0-9]+]]
// CHECK-DAG: ![[VB]] = !{i64 32, i64 6, i64 16, i64 0}
struct VBased : virtual V { virtual void f(); };
void VBased::f() {}

// The construction vtable of VBased in Derived takes the same rule.
// CHECK-DAG: @_ZTC7Derived0_6VBased = {{.*}} align 8, !pin ![[VB]]{{$}}
// A VTT is not placed.
// CHECK-DAG: @_ZTT7Derived = {{.*}} align 8{{$}}
struct Derived : VBased { virtual void g(); };
void Derived::g() {}

#ifndef NO_TAGS
// Final in effect, with no key function: tagged, a required pin, and no slot
// in its base's run, though its record names the base, so that the linker can
// place it among the run.
// CHECK-DAG: @_ZTV6Tagged = linkonce_odr {{.*}} align 8, !pin ![[TAG:[0-9]+]], !vtable_hierarchy ![[TAGH:[0-9]+]]{{$}}
// CHECK-DAG: ![[TAGH]] = !{i64 16, !"6Tagged", !"5Plain"}
// The residue is 64 * line + 24 + 8 * k for xxh3_64bits("_ZTS6Tagged") = h,
// line = h >> 58 = 6 and k = (5 * (h & (2^58 - 1))) >> 58 = 2.
// CHECK-DAG: ![[TAG]] = !{i64 16, i64 12, i64 424, i64 1}
struct Tagged final : Plain { void f() override {} };

// Tagged with a virtual base: the pin is on the address point at 32.
// CHECK-DAG: @_ZTV7TaggedV = linkonce_odr {{.*}} align 8, !pin ![[TAGV:[0-9]+]], !vtable_hierarchy {{![0-9]+$}}
// CHECK-DAG: ![[TAGV]] = !{i64 32, i64 12, i64 544, i64 1}
struct TaggedV final : virtual VI { virtual void g() {} };

// Hidden classes are tagged too: visibility can differ between units.
// CHECK-DAG: @_ZTV6Hidden = linkonce_odr hidden {{.*}} align 8, !pin ![[HID:[0-9]+]], !vtable_hierarchy {{![0-9]+$}}
// CHECK-DAG: ![[HID]] = !{i64 16, i64 12, i64 3432, i64 1}
struct __attribute__((visibility("hidden"))) Hidden final : Plain {
  void f() override {}
};

// A template instantiation has no key function.
// CHECK-DAG: @_ZTV4TmplIiE = linkonce_odr {{.*}} align 8, !pin ![[TMPL:[0-9]+]], !vtable_hierarchy {{![0-9]+$}}
// CHECK-DAG: ![[TMPL]] = !{i64 16, i64 12, i64 2328, i64 1}
template <typename T> struct Tmpl final : Plain { void f() override {} };

// A class in an anonymous namespace is not tagged.
// CHECK-DAG: @_ZTVN12_GLOBAL__N_15LocalE = internal {{.*}} align 64, !pin ![[K0]], !vtable_hierarchy
namespace {
struct Local final : Plain { void f() override {} };
}

// A key function carrying a DLL attribute is a key function in every image:
// the class is not tagged.
// CHECK-DAG: @_ZTV8Exported = {{.*}}constant {{.*}} align 64, !pin ![[K0]], !vtable_hierarchy
struct Exported final : Plain {
  __attribute__((dllexport)) virtual void g();
  void f() override {}
};
void Exported::g() {}

void use() {
  Tagged t;
  TaggedV tv;
  Hidden h;
  Tmpl<int> tm;
  Local l;
}

// CHECK-DAG: !{!"/FAILIFMISMATCH:\22_WIN_ITANIUM_VTABLE_LAYOUT=1\22"}
// CHECK-DAG: !{!"/INCLUDE:__llvm_link_pins_v1"}
// A joint link keeps one copy of each tagged vtable, which its scan learns
// from these, bitcode included.
// CHECK-DAG: !{!"/EXACTCAST:_ZTV6Tagged"}
// CHECK-DAG: !{!"/EXACTCAST:_ZTV7TaggedV"}
// CHECK-DAG: !{!"/EXACTCAST:_ZTV6Hidden"}
// CHECK-DAG: !{!"/EXACTCAST:_ZTV4TmplIiE"}
// FACT-NOT: /EXACTCAST:_ZTVN12_GLOBAL__N_15LocalE
// FACT-NOT: /EXACTCAST:_ZTV8Exported
// FACT-NOT: /EXACTCAST:_ZTV5Plain
#endif

// Without a tagged class there is no required pin and no directive.
// NOTAG-NOT: = !{i64 {{[0-9]+}}, i64 12,
// NOTAG-NOT: _WIN_ITANIUM_VTABLE_LAYOUT
// NOTAG-NOT: __llvm_link_pins_v1
// NOTAG-NOT: /EXACTCAST

// ELF-NOT: !pin
// ELF: @_ZTV5Plain = {{.*}} align 8{{$}}
