//===-- dtor_call.cpp - Exception-safe destructor invocation --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Destructor invocation with exception handling using SEH.
//
// Uses native Windows SEH (__try/__except) instead of C++ try/catch to avoid
// circular dependencies: C++ exceptions require libunwind and libc++abi, but
// wincrt must be usable before those are built.
//
// SEH catches exceptions at the OS level, which includes:
// - C++ exceptions (SEH exceptions with Itanium-specific payload)
// - Hardware exceptions (access violations, etc.)
//
// Per Itanium ABI 3.3.5: if a destructor throws during __cxa_finalize,
// terminate() must be called. We implement this by catching at the SEH level
// and calling abort().
//
//===----------------------------------------------------------------------===//

#ifdef LLVM_RUNTIME_WIN32

extern "C" {

__declspec(noreturn) void __cdecl abort(void);
__declspec(dllimport) void __stdcall OutputDebugStringA(const char*);

}

namespace wincrt {

// Called from cxa_atexit.cpp and cxa_thread_atexit.cpp.
// Uses SEH to catch any exception (C++ or otherwise) and terminate.
void invokeDestructorImpl(void (*dtor)(void*), void* obj,
                          const char* context) {
  __try {
    dtor(obj);
  } __except(1) {  // EXCEPTION_EXECUTE_HANDLER = 1
    OutputDebugStringA("WINCRT FATAL: destructor threw during ");
    OutputDebugStringA(context);
    OutputDebugStringA("\n");
    // Per Itanium ABI, call terminate. SEH caught the exception at the OS
    // level, so we just abort - this is equivalent to std::terminate().
    abort();
  }
}

} // namespace wincrt

#endif // LLVM_RUNTIME_WIN32
