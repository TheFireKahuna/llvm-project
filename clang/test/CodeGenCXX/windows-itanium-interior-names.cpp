// DEFINE: %{cc} = %clang_cc1 -std=c++17 -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s
// DEFINE: %{only} = --implicit-check-not='{{(hidden|internal|tls|array|weak|Hidden|Final)[$]}}'
// RUN: %{cc} -triple x86_64-unknown-windows-itanium | FileCheck %s %{only}
// RUN: %{cc} -triple aarch64-pc-windows-ntposix | FileCheck %s %{only}
// RUN: %{cc} -triple x86_64-w64-mingw32 | FileCheck %s --check-prefix=MINGW

/// The loader adds no offset to an imported address, so a definition another
/// image can reach names its interior addresses as aliases: a vtable's address
/// points ("$ap"), a class variable's fixed subobjects ("$so"). A hidden,
/// internal, thread-local, undefined or non-COMDAT weak entity has none.

// MINGW-NOT: {{[$](ap|so)}}

#define DEFAULT __attribute__((visibility("default")))
#define PROTECTED __attribute__((visibility("protected")))
#define HIDDEN __attribute__((visibility("hidden")))

struct Single {
  virtual void key();
  int a;
};
void Single::key() {}

struct Second {
  virtual void second();
  int s;
};
struct VBase {
  virtual void vbase();
  int v;
};
struct DEFAULT Group : Single, Second, virtual VBase {
  void key() override;
};
void Group::key() {}
struct Vague : Single {
  void key() override {}
};
struct PROTECTED Bound {
  virtual void key();
};
void Bound::key() {}
struct HIDDEN Hidden {
  virtual void key();
};
void Hidden::key() {}
struct Final final : Single {
  void key() override;
};

struct Inner {
  int x;
  Second second;
};
struct Members : Single, virtual VBase {
  int first;
  Inner inner;
  int array[2];
  Inner innerArray[2];
  int &ref = first;
  int bits : 3;
  int last;
};
DEFAULT Members members;
PROTECTED Inner bound;
HIDDEN Inner hidden;
static Inner internal;
thread_local Inner tls;
inline Inner inlineVar;
Inner array[2];
__attribute__((weak)) Inner weak;
inline Inner &local() {
  static Inner s;
  return s;
}
void *use() {
  Vague v;
  Final f;
  (void)internal;
  (void)inlineVar;
  (void)tls;
  return &local();
}
int *address = &members.inner.second.s;

// CHECK:      @address = dso_local global ptr getelementptr (i8, ptr @members, i64 32)

// CHECK:      @"members$so8" = dso_local dllexport alias i8, getelementptr inbounds (i8, ptr @members, i64 8)
// CHECK-NEXT: @"members$so12" =
// CHECK-NEXT: @"members$so16" =
// CHECK-NEXT: @"members$so24" =
// CHECK-NEXT: @"members$so32" =
// CHECK-NEXT: @"members$so40" =
// CHECK-NEXT: @"members$so48" =
// CHECK-NEXT: @"members$so108" =
// CHECK-NEXT: @"members$so112" =
// CHECK-NEXT: @"members$so120" =
// CHECK-NEXT: @"bound$so8" = protected dllexport alias i8, getelementptr inbounds (i8, ptr @bound, i64 8)
// CHECK-NEXT: @"bound$so16" =
// CHECK-NEXT: @"inlineVar$so8" = alias i8, getelementptr inbounds (i8, ptr @inlineVar, i64 8)
// CHECK-NEXT: @"inlineVar$so16" =
// CHECK-NEXT: @"_ZTV6Single$ap16" = dso_local alias i8, getelementptr inbounds (i8, ptr @_ZTV6Single, i64 16)
// CHECK-NEXT: @"_ZTV5Group$ap24" = dso_local dllexport alias i8, getelementptr inbounds (i8, ptr @_ZTV5Group, i64 24)
// CHECK-NEXT: @"_ZTV5Group$ap48" = dso_local dllexport alias
// CHECK-NEXT: @"_ZTV5Group$ap80" = dso_local dllexport alias
// CHECK-NEXT: @"_ZTV5Bound$ap16" = protected dllexport alias
// CHECK-NEXT: @"_ZTV7Members$ap24" = dso_local unnamed_addr alias
// CHECK-NEXT: @"_ZTV7Members$ap56" = dso_local unnamed_addr alias
// CHECK-NEXT: @"_ZTV5Vague$ap16" = dso_local unnamed_addr alias
// CHECK-NEXT: @"_ZZ5localvE1s$so8" = alias
// CHECK-NEXT: @"_ZZ5localvE1s$so16" = alias
// CHECK-NEXT: @_ZTH3tls =
// CHECK:      store ptr getelementptr inbounds inrange(-16, 8) (i8, ptr @_ZTV5Final, i64 16)
