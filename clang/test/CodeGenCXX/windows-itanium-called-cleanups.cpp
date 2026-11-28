// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -exception-model=seh -fexceptions -fcxx-exceptions -std=c++14 -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -exception-model=seh -fexceptions -fcxx-exceptions -std=c++14 -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-w64-windows-gnu -exception-model=seh -fexceptions -fcxx-exceptions -std=c++14 -emit-llvm -o - %s | FileCheck %s --check-prefix=GNU

// On Windows Itanium the unwinder calls a cleanup and chooses the handler it
// lands in: a cleanup is a cleanuppad, and a try block a catchswitch with a
// catchpad for each handler, which the handler's code follows by a catchret
// with the exception object's address. Elsewhere the same personality lands
// landing pads.

struct A { A(); ~A(); };
struct B { ~B() noexcept(false); };
void may_throw();

// CHECK-LABEL: define dso_local void @_Z7cleanupv()
// CHECK-SAME:    personality ptr @__gxx_personality_seh0
// CHECK:         invoke void @_Z9may_throwv()
// CHECK-NEXT:      to label %invoke.cont unwind label %ehcleanup
// CHECK:       ehcleanup:
// CHECK-NEXT:    %[[PAD:.*]] = cleanuppad within none []
// CHECK-NEXT:    call void @_ZN1AD1Ev(ptr {{.*}} %a) #{{[0-9]+}} [ "funclet"(token %[[PAD]]) ]
// CHECK-NEXT:    cleanupret from %[[PAD]] unwind to caller
//
// GNU-LABEL: define dso_local void @_Z7cleanupv()
// GNU:         landingpad { ptr, i32 }
// GNU-NEXT:      cleanup
// GNU-NOT:     cleanuppad
void cleanup() {
  A a;
  may_throw();
}

// Try blocks with cleanups between them: each scope's pad unwinds to the pad
// of the scope around it. A handler's code is the function's: its calls carry
// no funclet bundle and unwind to the scope around the try block, here the
// handler's own end of the catch.
// CHECK-LABEL: define dso_local void @_Z6nestedv()
// CHECK:       ehcleanup:
// CHECK-NEXT:    %[[PB:.*]] = cleanuppad within none []
// CHECK-NEXT:    call void @_ZN1AD1Ev(ptr {{.*}} %b) #{{[0-9]+}} [ "funclet"(token %[[PB]]) ]
// CHECK-NEXT:    cleanupret from %[[PB]] unwind label %[[INNER:.*]]
// CHECK:       [[INNER]]:
// CHECK-NEXT:    %[[SI:.*]] = catchswitch within none [label %[[INT:.*]]] unwind label %[[CLEANUPA:.*]]
// CHECK:       [[INT]]:
// CHECK-NEXT:    %[[CI:.*]] = catchpad within %[[SI]] [ptr @_ZTIi]
// CHECK-NEXT:    %[[EXN:.*]] = call ptr @llvm.eh.exceptionpointer.p0(token %[[CI]])
// CHECK-NEXT:    catchret from %[[CI]] to label %[[BODY:.*]]
// CHECK:       [[BODY]]:
// CHECK-NEXT:    store ptr %[[EXN]], ptr %exn.slot
// CHECK-NEXT:    %[[E:.*]] = load ptr, ptr %exn.slot
// CHECK-NEXT:    call ptr @__cxa_begin_catch(ptr %[[E]])
// CHECK:       [[CLEANUPA]]:
// CHECK-NEXT:    %[[PA:.*]] = cleanuppad within none []
// CHECK-NEXT:    call void @_ZN1AD1Ev(ptr {{.*}} %a) #{{[0-9]+}} [ "funclet"(token %[[PA]]) ]
// CHECK-NEXT:    cleanupret from %[[PA]] unwind label %[[OUTER:.*]]
// CHECK:       [[OUTER]]:
// CHECK-NEXT:    %[[SO:.*]] = catchswitch within none [label %[[ALL:.*]]] unwind to caller
// CHECK:       [[ALL]]:
// CHECK-NEXT:    catchpad within %[[SO]] [ptr null]
// CHECK:         call ptr @__cxa_begin_catch(
// CHECK-NEXT:    invoke void @_Z9may_throwv()
// CHECK-NEXT:      to label %{{.*}} unwind label %[[END:.*]]
// CHECK:       [[END]]:
// CHECK-NEXT:    %[[PE:.*]] = cleanuppad within none []
// CHECK-NEXT:    call void @__cxa_end_catch() [ "funclet"(token %[[PE]]) ]
// CHECK-NEXT:    cleanupret from %[[PE]] unwind to caller
void nested() {
  try {
    A a;
    try {
      A b;
      may_throw();
    } catch (int) {
    }
  } catch (...) {
    may_throw();
  }
}

// An exception specification is a pad that names the types it allows, whose
// code calls __cxa_call_unexpected with the exception.
// CHECK-LABEL: define dso_local void @_Z4specv()
// CHECK:         cleanupret from %{{.*}} unwind label %filter.dispatch
// CHECK:       filter.dispatch:
// CHECK-NEXT:    %[[S:.*]] = catchswitch within none [label %ehspec.unexpected] unwind to caller
// CHECK:       ehspec.unexpected:
// CHECK-NEXT:    %[[P:.*]] = catchpad within %[[S]] {{\[}}[1 x ptr] [ptr @_ZTIi]]
// CHECK-NEXT:    %[[EXN:.*]] = call ptr @llvm.eh.exceptionpointer.p0(token %[[P]])
// CHECK-NEXT:    catchret from %[[P]] to label %[[BODY:.*]]
// CHECK:       [[BODY]]:
// CHECK-NEXT:    call void @__cxa_call_unexpected(ptr %[[EXN]])
// CHECK-NEXT:    unreachable
void spec() throw(int) {
  A a;
  may_throw();
}

// A terminate scope is a pad that takes every exception and hands it to
// __clang_call_terminate, which marks it caught.
// CHECK-LABEL: define dso_local void @_Z9terminatev()
// CHECK:       terminate:
// CHECK-NEXT:    %[[S:.*]] = catchswitch within none [label %terminate.handler] unwind to caller
// CHECK:       terminate.handler:
// CHECK-NEXT:    %[[P:.*]] = catchpad within %[[S]] [ptr null]
// CHECK-NEXT:    %[[EXN:.*]] = call ptr @llvm.eh.exceptionpointer.p0(token %[[P]])
// CHECK-NEXT:    catchret from %[[P]] to label %[[BODY:.*]]
// CHECK:       [[BODY]]:
// CHECK-NEXT:    call void @__clang_call_terminate(ptr %[[EXN]])
void terminate() noexcept {
  A a;
  may_throw();
}

// Nothing unwinds out of a cleanup: a destructor that may throw is a call
// there, and an invoke on the normal path.
// CHECK-LABEL: define dso_local void @_Z19throwing_destructorv()
// CHECK:         invoke void @_ZN1BD1Ev(
// CHECK:       ehcleanup:
// CHECK-NEXT:    %[[PAD:.*]] = cleanuppad within none []
// CHECK-NEXT:    call void @_ZN1BD1Ev(ptr {{.*}} %b) [ "funclet"(token %[[PAD]]) ]
void throwing_destructor() {
  A a;
  B b;
  may_throw();
}

// The implicit rethrow at the end of a constructor's function-try-block
// leaves the handler through its end of the catch.
// CHECK-LABEL: define dso_local void @_ZN1CC2Ev(
// CHECK:         catchpad within %{{.*}} [ptr null]
// CHECK:         call ptr @__cxa_begin_catch(
// CHECK-NEXT:    invoke void @__cxa_rethrow()
// CHECK-NEXT:      to label %{{.*}} unwind label %[[END:.*]]
// CHECK:       [[END]]:
// CHECK-NEXT:    %[[PE:.*]] = cleanuppad within none []
// CHECK-NEXT:    call void @__cxa_end_catch() [ "funclet"(token %[[PE]]) ]
struct C {
  A a;
  C();
};
C::C() try : a() {
} catch (...) {
}
