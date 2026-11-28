// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -fms-extensions -fcxx-exceptions -fexceptions -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -fms-extensions -fcxx-exceptions -fexceptions -emit-llvm -o - %s | FileCheck %s

// In C++ too a function of the C convention is passed its thread pointer,
// whether its name is a C name or a mangled one; code generation gives either
// its plain entry, which is its address.

struct S {
  S();
  ~S();
  int member(int);
};
int ext(int);

// CHECK: define dso_local noundef i32 @_ZN1S6memberEi({{.*}}) #[[ARG:[0-9]+]]
int S::member(int x) { return ext(x); }

// CHECK: define dso_local i32 @c_name(i32 noundef %x) #[[ARG]]
extern "C" int c_name(int x) { return ext(x); }

// CHECK: define dso_local noundef i32 @labelled(i32 noundef %x) #[[ARG]]
int with_label(int x) __asm__("labelled");
int with_label(int x) { return ext(x); }

// CHECK: define dso_local noundef i32 @main() #[[MAIN:[0-9]+]]
int main() { return 0; }

// A runtime function another image provides is passed it; the personality,
// which the unwinder calls, is not.
// CHECK-LABEL: define dso_local void @_Z6throw_v()
// CHECK: declare dllimport void @__cxa_throw(ptr, ptr, ptr) #[[RTARG:[0-9]+]]
void throw_() { throw 1; }
// CHECK-LABEL: define dso_local void @_Z7cleanupv()
// CHECK: declare dso_local i32 @__gxx_personality_v0(...){{$}}
void cleanup() {
  S s;
  ext(0);
}

// Every access of a thread-local calls its wrapper, which is passed it.
// CHECK: define linkonce_odr hidden noundef ptr @_ZTW2tv() #[[WRAPPER:[0-9]+]]
extern thread_local int tv;
int read_tv() { return tv; }

// A function the compiler makes is passed none.
// CHECK: define internal void @__cxx_global_var_init() #[[MADE:[0-9]+]]
S global;

// A function that a program replaces for every image is one body at its
// address, which the program's table holds, so neither it nor a call of it
// is passed one.
// CHECK: define dso_local noundef nonnull ptr @_Znwm(i64 noundef %n) #[[REPLACED:[0-9]+]]
// CHECK: call noalias noundef nonnull ptr @_Znwm(i64 noundef 4) #[[NEWCALL:[0-9]+]]
void *operator new(decltype(sizeof 0) n) { return (void *)(long)ext(n); }
int *make() { return new int; }

// CHECK-NOT: @"\01

// CHECK: attributes #[[ARG]] = {{{.*}} "thread-pointer-argument" }
// CHECK: attributes #[[MAIN]] = {{{.*}} "thread-pointer-argument" }
// CHECK: attributes #[[RTARG]] = { "thread-pointer-argument" }
// CHECK: attributes #[[WRAPPER]] = {{{.*}} "thread-pointer-argument" }
// CHECK: attributes #[[MADE]] = {
// CHECK-NOT: thread-pointer
// CHECK-SAME: {{$}}
// CHECK: attributes #[[REPLACED]] = {
// CHECK-NOT: thread-pointer
// CHECK-SAME: {{$}}
// CHECK: attributes #[[NEWCALL]] = {
// CHECK-NOT: thread-pointer
// CHECK-SAME: {{$}}
