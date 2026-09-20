// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -mdefault-visibility-export-mapping=explicit -fno-auto-import -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -mdefault-visibility-export-mapping=explicit -fno-auto-import -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -mdefault-visibility-export-mapping=explicit -fno-auto-import -DDEFINE_KEY_FUNCTIONS -emit-llvm -o - %s | FileCheck %s --check-prefix=PRODUCER
// RUN: %clang_cc1 -triple x86_64-w64-windows-gnu -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | FileCheck %s --check-prefix=MINGW

// A vtable in another image is reached through a pointer the loader fills,
// and static data can hold that pointer's value but not an offset from it.
// The image that defines the vtable gives each of its address points a name,
// and an image that imports the vtable uses those names.

struct __attribute__((visibility("default"))) Base {
  virtual int value();
};

struct __attribute__((visibility("default"))) Virtual : virtual Base {
  virtual int other();
};

#ifdef DEFINE_KEY_FUNCTIONS

int Base::value() { return 1; }
int Virtual::other() { return 2; }

// PRODUCER: @_ZTV4Base = dso_local dllexport unnamed_addr constant
// PRODUCER: @_ZTV7Virtual = dso_local dllexport unnamed_addr constant
// PRODUCER: @"_ZTV4Base$ap16" = dllexport unnamed_addr alias i8, getelementptr inbounds (i8, ptr @_ZTV4Base, i32 16)
// PRODUCER: @"_ZTV7Virtual$ap32" = dllexport unnamed_addr alias i8, getelementptr inbounds (i8, ptr @_ZTV7Virtual, i32 32)

#else

Base Object;

// CHECK: @"_ZTV4Base$ap16" = external dllimport unnamed_addr constant i8, align 8
// CHECK: @Object = dso_local dllexport global %struct.Base { ptr @"_ZTV4Base$ap16" }

// A vtable this image defines is reached directly, as before.
struct Local : Base {
  int value() override { return 3; }
};
Local LocalObject;

// CHECK: @_ZTV5Local = linkonce_odr dso_local unnamed_addr constant
// CHECK: @LocalObject = dso_local global { ptr } { ptr getelementptr inbounds inrange(-16, 8) ({ [3 x ptr] }, ptr @_ZTV5Local, i32 0, i32 0, i32 2) }

// Other COFF targets keep the offset from the vtable itself.
// MINGW: @_ZTV4Base = external dllimport unnamed_addr constant
// MINGW: @Object = dso_local dllexport global %struct.Base { ptr getelementptr inbounds inrange(-16, 8) ({ [3 x ptr] }, ptr @_ZTV4Base, i32 0, i32 0, i32 2) }
// MINGW-NOT: $ap

#endif
