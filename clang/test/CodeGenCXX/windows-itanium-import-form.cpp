// DEFINE: %{cc} = %clang_cc1 -fno-function-type-prefix -fcxx-exceptions \
// DEFINE:   -fexceptions -fms-extensions -emit-llvm -o - %s
// RUN: %{cc} -triple x86_64-unknown-windows-itanium -fno-plt \
// RUN:   | FileCheck %s --check-prefixes=CHECK,NOPLT
// RUN: %{cc} -triple aarch64-pc-windows-ntposix \
// RUN:   | FileCheck %s --check-prefixes=CHECK,PLT
// RUN: %{cc} -triple x86_64-w64-windows-gnu -fno-plt \
// RUN:   | FileCheck %s --check-prefix=MINGW
// RUN: %{cc} -triple x86_64-pc-windows-ntposix -pic-level 2 \
// RUN:   | FileCheck %s --check-prefixes=DATA,IMPORT,OWNED
// RUN: %{cc} -triple aarch64-unknown-windows-itanium -pic-level 2 \
// RUN:   | FileCheck %s --check-prefixes=DATA,IMPORT,OWNED
// RUN: %{cc} -triple x86_64-unknown-windows-itanium -pic-level 2 \
// RUN:   -fdirect-access-external-data | FileCheck %s --check-prefixes=DATA,DIRECT
// RUN: %{cc} -triple x86_64-unknown-windows-itanium \
// RUN:   | FileCheck %s --check-prefixes=DATA,DIRECT

// On Windows Itanium and NT-POSIX a function the unit only declares is not
// dso_local, as on ELF, and under -fno-plt is nonlazybind with RtLibUseGOT.
// Hidden, internal, dllexport, later-defined and per-image functions stay
// dso_local. A runtime function is a known import under -fplt too. Other
// Windows targets are unchanged.

void plain();
__attribute__((visibility("hidden"))) void hidden();
__attribute__((weak)) void weak();
__declspec(dllimport) void imported();
__declspec(dllexport) void exported();
void later();
static void internal() {}

// A variable the unit only declares may be another image's too, unless
// -fdirect-access-external-data (cc1's default without a PIC level) says
// otherwise, as on ELF. Hidden and dllexport variables, and the vtable of a
// class the mapping does not export, are this image's. A thread-local
// variable that may be another image's is reached through its record, and is
// not declared.

extern int data;
extern const int constant;
__attribute__((visibility("hidden"))) extern int hidden_data;
__declspec(dllimport) extern int imported_data;
__declspec(dllexport) extern int exported_data;
extern thread_local int local_to_thread;
struct Unmarked {
  virtual ~Unmarked();
};

// IMPORT-DAG: @data = external global i32
// IMPORT-DAG: @constant = external constant i32
// DIRECT-DAG: @data = external dso_local global i32
// DIRECT-DAG: @constant = external dso_local constant i32
// DATA-DAG:   @hidden_data = external hidden global i32
// DATA-DAG:   @imported_data = external dllimport global i32
// DATA-DAG:   @exported_data = external dso_local global i32
// OWNED-DAG:  @local_to_thread = external thread_local global i32
// DIRECT-DAG: @local_to_thread = external dso_local thread_local global i32
// DATA-DAG:   @_ZTV8Unmarked = external dso_local constant

int read() {
  Unmarked u;
  return data + constant + hidden_data + imported_data + exported_data +
         local_to_thread;
}

// NOPLT-DAG: declare void @_Z5plainv() [[NOPLT:#[0-9]+]]
// PLT-DAG:   declare void @_Z5plainv() [[PLT:#[0-9]+]]
// CHECK-DAG: declare hidden void @_Z6hiddenv()
// CHECK-DAG: declare extern_weak void @_Z4weakv()
// CHECK-DAG: declare dllimport void @_Z8importedv()
// CHECK-DAG: declare dso_local void @_Z8exportedv()
// CHECK-DAG: define dso_local void @_Z5laterv()
// CHECK-DAG: define internal void @_ZL8internalv()
// CHECK-DAG: declare dso_local i32 @__gxx_personality_v0(...)
// CHECK-DAG: declare dllimport ptr @__cxa_allocate_exception(i64)
// NOPLT-DAG: attributes [[NOPLT]] = {{{.*}}nonlazybind
// NOPLT-DAG: !{i32 7, !"RtLibUseGOT", i32 1}
// PLT-NOT:   nonlazybind
// PLT-NOT:   RtLibUseGOT

// MINGW-DAG: declare dso_local void @_Z5plainv()
// MINGW-DAG: declare dso_local ptr @__cxa_allocate_exception(i64)

void use() {
  try {
    plain();
  } catch (...) {
  }
  hidden();
  if (weak)
    weak();
  imported();
  exported();
  later();
  internal();
  throw 1;
}

void later() {}
