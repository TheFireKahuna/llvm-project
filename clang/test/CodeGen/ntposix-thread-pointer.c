// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -fms-extensions -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -fms-extensions -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions -emit-llvm -o - %s | FileCheck --check-prefix=ITANIUM %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions -emit-llvm -o - %s | FileCheck --check-prefix=ITANIUM %s

// On NT-POSIX a function of the C convention is passed its thread pointer;
// code generation gives it the plain entry that code passing none calls.
// Windows Itanium marks nothing.
// ITANIUM-NOT: "thread-pointer
// ITANIUM-NOT: "no-thread-pointer

int ext(int);
int vext(const char *, ...);
__SIZE_TYPE__ strlen(const char *);
__attribute__((thread_pointer("read"))) void *alloc(__SIZE_TYPE__);

// CHECK: @also = dso_local alias i32 (i32), ptr @passed

// CHECK: define dso_local i32 @passed(i32 noundef %x) #[[ARG:[0-9]+]]
// CHECK: call i32 @ext(
// CHECK: declare {{.*}}i32 @ext(i32 noundef) #[[DECL:[0-9]+]]
int passed(int x) { return ext(x); }
int also(int) __attribute__((alias("passed")));

// CHECK: define internal i32 @helper(i32 noundef %x) #[[ARG]]
static int helper(int x) { return ext(x); }
int calls_helper(int x) { return helper(x); }

// CHECK: define dso_local i32 @variadic(ptr noundef %fmt, ...) #[[ARG]]
int variadic(const char *fmt, ...) { return vext(fmt, 1); }

// A function of Windows's convention, whatever its spelling, is passed none
// and has no plain entry.
// CHECK: define dso_local win64cc i32 @windows(i32 noundef %x) #[[NONE:[0-9]+]]
// CHECK: define dso_local win64cc i32 @windows_too(i32 noundef %x) #[[NONE]]
__attribute__((ms_abi)) int windows(int x) { return ext(x); }
int __stdcall windows_too(int x) { return ext(x); }

// CHECK: define dso_local i32 @probes(i32 noundef %x) #[[PROBE:[0-9]+]]
// CHECK: define dso_local i32 @derives(i32 noundef %x) #[[NONE]]
// CHECK: define dso_local i32 @has_none(i32 noundef %x) #[[HASNONE:[0-9]+]]
__attribute__((thread_pointer("probe"))) int probes(int x) { return ext(x); }
__attribute__((thread_pointer("derive"))) int derives(int x) { return ext(x); }
__attribute__((thread_pointer("none"))) int has_none(int x) { return x; }

// "refusal" on a function passed its thread pointer is its plain entry's; on
// one passed none, the function's own.
// CHECK: define dso_local i64 @reports(i32 noundef %x) #[[REPORTS:[0-9]+]]
// CHECK: define dso_local win64cc i64 @reports_itself(i32 noundef %x) #[[REFUSAL:[0-9]+]]
__attribute__((thread_pointer("refusal"))) long long reports(int x) { return ext(x); }
__attribute__((thread_pointer("refusal"), ms_abi)) long long reports_itself(int x) { return ext(x); }

// CHECK: define dso_local void @naked() #[[NAKED:[0-9]+]]
__attribute__((naked)) void naked(void) { __asm__("ret"); }

// A call through a pointer is no different: a function's address is its plain
// entry.
// CHECK-LABEL: define dso_local i32 @through_pointer(
// CHECK: call i32 %{{[0-9]+}}(i32 noundef %{{[0-9]+}}){{$}}
int through_pointer(int (*f)(int), int x) { return f(x); }
// CHECK-LABEL: define dso_local i32 @through_windows(
// CHECK: call win64cc i32 %{{[0-9]+}}(i32 noundef %{{[0-9]+}}){{$}}
int through_windows(int (__stdcall *f)(int), int x) { return f(x); }

// A memory function is marked as any other; touching only its arguments'
// memory, it is called at its plain entry.
// CHECK-LABEL: define dso_local i64 @length(
// CHECK: call i64 @strlen(
// CHECK: declare {{.*}}i64 @strlen(ptr noundef) #[[STRLEN:[0-9]+]]
__SIZE_TYPE__ length(const char *s) { return strlen(s); }

// A declaration that says it reads its thread pointer.
// CHECK: declare {{.*}}ptr @alloc(i64 noundef) #[[READ:[0-9]+]]
void *allocates(void) { return alloc(8); }
// CHECK-NOT: @"\01

// CHECK: attributes #[[ARG]] = {{{.*}} "thread-pointer-argument" }
// CHECK: attributes #[[DECL]] = {{{.*}} "thread-pointer-argument" }
// CHECK: attributes #[[NONE]] = {
// CHECK-NOT: thread-pointer
// CHECK-SAME: {{$}}
// CHECK: attributes #[[PROBE]] = {{{.*}} "thread-pointer-probe" }
// CHECK: attributes #[[HASNONE]] = {{{.*}} "no-thread-pointer" {{.*}}}
// CHECK: attributes #[[REPORTS]] = {{{.*}} "thread-pointer-argument" "thread-pointer-plain-entry"="refusal" }
// CHECK: attributes #[[REFUSAL]] = {{{.*}} "thread-pointer-refusal" }
// CHECK: attributes #[[NAKED]] = { naked
// CHECK-NOT: thread-pointer
// CHECK-SAME: {{$}}
// CHECK: attributes #[[STRLEN]] = { nounwind {{.*}}"thread-pointer-argument" }
// CHECK: attributes #[[READ]] = {{{.*}} "thread-pointer-argument"="read" }
