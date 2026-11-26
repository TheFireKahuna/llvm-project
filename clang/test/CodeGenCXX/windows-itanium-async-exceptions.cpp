// REQUIRES: x86-registered-target, aarch64-registered-target
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -exception-model=seh -fexceptions -fcxx-exceptions -fasync-exceptions -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -exception-model=seh -fexceptions -fcxx-exceptions -fasync-exceptions -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -exception-model=seh -fexceptions -fcxx-exceptions -emit-llvm -o - %s | FileCheck %s --check-prefix=SYNC
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -exception-model=seh -fexceptions -fcxx-exceptions -fasync-exceptions -S -o - %s | FileCheck %s --check-prefix=ASM
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -exception-model=seh -fexceptions -fcxx-exceptions -fasync-exceptions -S -o /dev/null %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -exception-model=seh -fexceptions -fcxx-exceptions -fasync-exceptions -O2 -S -o /dev/null %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -exception-model=seh -fexceptions -fcxx-exceptions -fasync-exceptions -O2 -S -o /dev/null %s

// Under -fasync-exceptions a Windows Itanium function names the personality
// routine that takes the exceptions any instruction raises, and each of its
// EH scopes is bracketed by markers that unwind to the scope's EH pad. The
// mode is the function's: no module flag carries it.

// CHECK-NOT: eh-asynch
// SYNC-NOT: llvm.seh.
// SYNC-NOT: __gxx_personality_seha0

struct A { A(); ~A(); };
void may();
void quiet() noexcept;
extern int *p;

// An object's scope begins once it is constructed and ends ahead of its
// destructor's call. A call in the scope is an invoke whatever its callee
// throws, since an asynchronous exception may come out of it.
// CHECK-LABEL: define dso_local void @_Z6objectv()
// CHECK-SAME:    personality ptr @__gxx_personality_seha0
// CHECK:         call void @_ZN1AC1Ev(
// CHECK-NEXT:    invoke void @llvm.seh.scope.begin()
// CHECK-NEXT:            to label %{{.*}} unwind label %[[CLEANUP:.*]]
// CHECK:         store i32 1, ptr
// CHECK-NEXT:    invoke void @_Z5quietv()
// CHECK-NEXT:            to label %{{.*}} unwind label %[[CLEANUP]]
// CHECK:         invoke void @llvm.seh.scope.end()
// CHECK-NEXT:            to label %{{.*}} unwind label %[[CLEANUP]]
// CHECK:         call void @_ZN1AD1Ev(
// CHECK-NEXT:    ret void
// CHECK:       [[CLEANUP]]:
// CHECK-NEXT:    cleanuppad within none []
// SYNC-LABEL: define dso_local void @_Z6objectv()
// SYNC:          call void @_Z5quietv()
void object() {
  A a;
  *p = 1;
  quiet();
}

// Every try block is a scope, whose typed handler takes what a translator
// makes of an asynchronous exception, and each way out of the block ends it.
// CHECK-LABEL: define dso_local noundef i32 @_Z7handlerv()
// CHECK:         invoke void @llvm.seh.try.begin()
// CHECK-NEXT:            to label %{{.*}} unwind label %[[DISPATCH:.*]]
// CHECK:         store i32 2, ptr
// CHECK:       if.then:
// CHECK:         br label %[[END:.*]]
// CHECK:       if.end:
// CHECK:         br label %[[END]]
// CHECK:       [[END]]:
// CHECK-NEXT:    invoke void @llvm.seh.try.end()
// CHECK-NEXT:            to label %{{.*}} unwind label %[[DISPATCH]]
// CHECK:       [[DISPATCH]]:
// CHECK-NEXT:    catchswitch within none [label %[[CATCH:.*]]] unwind to caller
// CHECK:       [[CATCH]]:
// CHECK-NEXT:    catchpad within %{{.*}} [ptr @_ZTIi]
// CHECK-NOT:     llvm.seh.try.end
// CHECK:       }
int handler() {
  try {
    *p = 2;
    if (*p)
      return 1;
  } catch (int) {
    *p = 3;
  }
  return 0;
}

// A noexcept function keeps its terminate scope: a pad for an empty exception
// specification, which no C++ exception passes and the personality routine
// does not offer an asynchronous exception to. Such an exception passes the
// scope with its cleanup run; one that a handler of a caller this function
// was inlined into takes goes on to that handler. Without the flag the pad
// takes every exception.
// CHECK-LABEL: define dso_local void @_Z7nothrowv()
// CHECK:         invoke void @llvm.seh.scope.begin()
// CHECK-NEXT:            to label %{{.*}} unwind label %[[CLEANUP:.*]]
// CHECK:       terminate:
// CHECK-NEXT:    catchswitch within none [label %terminate.handler] unwind to caller
// CHECK:       terminate.handler:
// CHECK-NEXT:    catchpad within %{{.*}} {{\[}}[0 x ptr] zeroinitializer]
// CHECK:       [[CLEANUP]]:
// CHECK-NEXT:    %[[PAD:.*]] = cleanuppad within none []
// CHECK:         cleanupret from %[[PAD]] unwind label %terminate
// SYNC-LABEL: define dso_local void @_Z7nothrowv()
// SYNC:          catchpad within %{{.*}} [ptr null]
void nothrow() noexcept {
  A a;
  may();
}

// The flag of a cleanup that a conditional expression may leave undone is
// written where its value changes, for an exception at any instruction.
// CHECK-LABEL: define dso_local void @_Z11conditionalb(
// CHECK:         store volatile i1 false, ptr %cleanup.cond
// CHECK:         store volatile i1 true, ptr %cleanup.cond
// SYNC-LABEL: define dso_local void @_Z11conditionalb(
// SYNC-NOT:      volatile
// SYNC:        }
void use(const A &);
void conditional(bool c) {
  c ? use(A()) : may();
}

// A jump out of a partly constructed aggregate goes through the cleanup of
// what is constructed, which ends the cleanup's scope, and so does the way on
// that leaves the cleanup undone, where the scope of the whole begins: an
// exception after the aggregate's lifetime does not destroy its first element
// again.
// CHECK-LABEL: define dso_local void @_Z9aggregatev()
// CHECK:         invoke void @llvm.seh.scope.begin()
// CHECK-NEXT:            to label %{{.*}} unwind label %[[PARTIAL:.*]]
// CHECK:       if.then:
// CHECK-NEXT:    invoke void @llvm.seh.scope.end()
// CHECK-NEXT:            to label %{{.*}} unwind label %[[PARTIAL]]
// CHECK:         invoke void @_ZN1BC1Ei(ptr {{.*}} %arrayinit.element, i32 noundef %{{.*}})
// CHECK-NEXT:            to label %[[BUILT:.*]] unwind label %[[PARTIAL]]
// CHECK:       [[BUILT]]:
// CHECK-NEXT:    invoke void @llvm.seh.scope.end()
// CHECK-NEXT:            to label %[[WHOLE:.*]] unwind label %[[PARTIAL]]
// CHECK:       [[WHOLE]]:
// CHECK-NEXT:    invoke void @llvm.seh.scope.begin()
// CHECK-NOT:     unwind label %[[PARTIAL]]{{$}}
// CHECK:         store i32 4, ptr
struct B { B(int); ~B(); };
void aggregate() {
  { B b[2] = {1, ({ if (*p) return; 2; })}; }
  *p = 4;
}

// The copy of a catch parameter is made in a terminate scope, which an
// asynchronous exception passes by. A cleanup around it ends the exception
// that was being caught, where an unwind passes the frame.
// CHECK-LABEL: define dso_local void @_Z8by_valuev()
// CHECK:         %exn = load ptr, ptr %exn.slot
// CHECK:         invoke void @llvm.seh.scope.begin()
// CHECK-NEXT:            to label %{{.*}} unwind label %[[END:.*]]
// CHECK:         invoke void @_ZN1CC1ERKS_(
// CHECK-NEXT:            to label %{{.*}} unwind label %[[TERMINATE:.*]]
// CHECK:         invoke void @llvm.seh.scope.end()
// CHECK-NEXT:            to label %{{.*}} unwind label %[[END]]
// CHECK:         call ptr @__cxa_begin_catch(ptr %exn)
// CHECK:       [[TERMINATE]]:
// CHECK-NEXT:    catchswitch within none [label %[[HANDLER:.*]]] unwind label %[[END]]
// CHECK:       [[HANDLER]]:
// CHECK-NEXT:    catchpad within %{{.*}} {{\[}}[0 x ptr] zeroinitializer]
// CHECK:       [[END]]:
// CHECK-NEXT:    %[[PAD:.*]] = cleanuppad within none []
// CHECK-NEXT:    call ptr @__cxa_begin_catch(ptr %exn) {{.*}}[ "funclet"(token %[[PAD]]) ]
// CHECK-NEXT:    call void @__cxa_end_catch() {{.*}}[ "funclet"(token %[[PAD]]) ]
// CHECK-NEXT:    cleanupret from %[[PAD]] unwind to caller
// SYNC-LABEL: define dso_local void @_Z8by_valuev()
// SYNC:          catchpad within %{{.*}} [ptr null]
// SYNC:          call void @__clang_call_terminate(
struct C { C(const C &); ~C(); };
void by_value() {
  try {
    may();
  } catch (C c) {
  }
}

// A jump out of an EH scope ends it, whether or not
// the scope's cleanup runs on the way: a cleanup that runs for an exception
// only, as a new-expression's operator delete, is a normal cleanup that ends
// its scope and runs nothing, and the jump is threaded through it. Where the
// jump lands is in no scope that it left.

struct Q { Q(const A &, int); };
struct R { R(int); };
int g();

// A goto ahead, out of the scope of the operator delete and through the
// temporary's cleanup.
// ASM-LABEL: {{^}}_Z4jumpv$tp:
// ASM:       # %out
// ASM-NEXT:    movq $0,
Q *jump() {
  Q *q = new Q(A(), ({ if (g()) goto out; 1; }));
  return q;
out:
  return nullptr;
}

// Two such scopes, neither inside the other, meet where they are both ended.
// ASM-LABEL: {{^}}_Z8siblingsv$tp:
// ASM:       # %out
// ASM-NEXT:    movq p(%rip), %rax
// ASM-NEXT:    movl $5, (%rax)
void siblings() {
  new Q(A(), ({ if (g()) goto out; 1; }));
  new Q(A(), ({ if (g()) goto out; 2; }));
out:
  *p = 5;
}

// The jumps of a loop and a return.
// ASM-LABEL: {{^}}_Z4loopv$tp:
// ASM:       # %cleanup.cont
// ASM-NEXT:    #
// ASM-NEXT:    movq p(%rip), %rax
// ASM-NEXT:    movl $6, (%rax)
// ASM-NEXT:  # %for.inc
// ASM-NEXT:    #
// ASM-NEXT:    movl
// ASM:       # %for.end
// ASM-NEXT:    movq p(%rip), %rax
// ASM-NEXT:    movl $7, (%rax)
// ASM-NEXT:  # %return
void loop() {
  for (int i = 0; i < 2; ++i) {
    new Q(A(), ({ if (g()) break; if (g()) continue; if (g()) return; 1; }));
    *p = 6;
  }
  *p = 7;
}

// A jump out of two scopes with no cleanup to run ends the inner and then the
// outer.
// CHECK-LABEL: define dso_local void @_Z6nestedv()
// CHECK:         invoke void @llvm.seh.scope.begin()
// CHECK-NEXT:            to label %{{.*}} unwind label %[[OUTER:.*]]
// CHECK:         invoke void @llvm.seh.scope.begin()
// CHECK-NEXT:            to label %{{.*}} unwind label %[[INNER:.*]]
// CHECK:       if.then:
// CHECK-NEXT:    store i32 2, ptr %cleanup.dest.slot
// CHECK-NEXT:    invoke void @llvm.seh.scope.end()
// CHECK-NEXT:            to label %[[LEFT:.*]] unwind label %[[INNER]]{{$}}
// CHECK:       [[LEFT]]:
// CHECK-NEXT:    invoke void @llvm.seh.scope.end()
// CHECK-NEXT:            to label %[[OUT:.*]] unwind label %[[OUTER]]{{$}}
// CHECK:       [[OUT]]:
// CHECK-NEXT:    %cleanup.dest = load i32, ptr %cleanup.dest.slot
// CHECK-NEXT:    switch i32 %cleanup.dest, label %{{.*}} [
// CHECK-NEXT:      i32 2, label %out
// ASM-LABEL: {{^}}_Z6nestedv$tp:
// ASM:       # %out
// ASM-NEXT:    movq p(%rip), %rax
// ASM-NEXT:    movl $8, (%rax)
void nested() {
  new R(({ new R(({ if (g()) goto out; 1; })); 2; }));
out:
  *p = 8;
}
