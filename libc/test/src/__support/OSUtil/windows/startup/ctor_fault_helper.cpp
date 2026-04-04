//===-- Helper DLL: faulting static ctor under loader lock ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Helper DLL for ctor_faults_during_startup_test. The parent test dlopen()'s
// this module, which causes the loader to invoke this DLL's static
// constructors WHILE HOLDING THE LOADER LOCK. The constructor below
// deliberately performs a null-pointer store so we can assert that the libc
// master VEH (re-registered front-of-chain by the LDR_DLL_NOTIFICATION_REASON_
// LOADED callback in veh_core.cpp::dll_notify_callback) is the dispatcher
// that observes the fault.
//
// Faulting is gated by an environment variable so this DLL can be loaded for
// other reasons (e.g., the test process itself accidentally pulling it in via
// DT_NEEDED equivalents) without immediately crashing. The variable is read
// directly from the PEB process-parameters block to avoid any libc dependency
// — keeping this DLL freestanding so the fault originates strictly in
// third-party code, not in libc.
//
// CMake wiring needed (deferred — this file deliberately does NOT touch any
// CMakeLists.txt per the task constraint):
//
//   * Build as a SHARED library named "ctor_fault_helper.dll", placed
//     alongside the parent test executable's RUNTIME_OUTPUT_DIRECTORY
//     (mirroring the dlopen_segv_helper pattern, but with a static ctor
//     instead of an exported entry).
//   * Must NOT link c.lib / libc_shared. Bare DLL with DllMain returning TRUE.
//   * Parent test target must depend on this DLL and define
//     CTOR_FAULT_HELPER_PATH=$<TARGET_FILE:ctor_fault_helper>.
//
//===----------------------------------------------------------------------===//

#if !defined(_WIN32) && !defined(_WIN32_ITANIUM) && !defined(__NTPOSIX__)
#error "ctor_fault_helper is Windows-only"
#endif

#define HELPER_EXPORT [[gnu::dllexport]]

namespace {

// Read the environment variable directly from PEB->ProcessParameters->
// Environment without going through any libc or Win32 surface — this is
// the same access pattern used by the NT-POSIX libc itself (see
// temp_path.h: NtCurrentPeb()->ProcessParameters->Environment).
//
// The environment block is a UTF-16 sequence of "NAME=VALUE\0" records
// terminated by an empty string ("\0"). We only test presence; the
// value does not matter.
#if defined(__x86_64__) || defined(_M_X64)
static inline void *nt_peb() {
  // TEB.ProcessEnvironmentBlock at gs:0x60.
  void *peb;
  __asm__ volatile("movq %%gs:0x60, %0" : "=r"(peb));
  return peb;
}
#elif defined(__aarch64__) || defined(_M_ARM64)
static inline void *nt_peb() {
  // TEB self at x18; TEB.ProcessEnvironmentBlock at x18+0x60.
  void *peb;
  __asm__ volatile("ldr %0, [x18, #0x60]" : "=r"(peb));
  return peb;
}
#else
#error "unsupported architecture"
#endif

// Case-insensitive ASCII compare between a UTF-16 env-block name and an
// ASCII query name. Env-var names are ASCII in practice.
bool env_var_is_set(const char *name) {
  unsigned char *peb = static_cast<unsigned char *>(nt_peb());
  if (!peb)
    return false;
  // PEB::ProcessParameters at +0x20.
  void *params = *reinterpret_cast<void **>(peb + 0x20);
  if (!params)
    return false;
  // RTL_USER_PROCESS_PARAMETERS::Environment at +0x80.
  const unsigned short *env = *reinterpret_cast<const unsigned short **>(
      static_cast<unsigned char *>(params) + 0x80);
  if (!env)
    return false;

  unsigned long name_len = 0;
  while (name[name_len])
    ++name_len;

  while (*env) {
    const unsigned short *p = env;
    while (*p && *p != u'=')
      ++p;
    unsigned long this_nlen = static_cast<unsigned long>(p - env);
    if (this_nlen == name_len && *p == u'=') {
      bool match = true;
      for (unsigned long i = 0; i < name_len; ++i) {
        unsigned short a = env[i];
        unsigned short b = static_cast<unsigned short>(name[i]);
        if (a >= u'a' && a <= u'z')
          a = static_cast<unsigned short>(a - 32);
        if (b >= u'a' && b <= u'z')
          b = static_cast<unsigned short>(b - 32);
        if (a != b) {
          match = false;
          break;
        }
      }
      if (match)
        return true;
    }
    while (*env)
      ++env;
    ++env; // past NUL
  }
  return false;
}

// File-scope object whose constructor runs at DLL_PROCESS_ATTACH, under the
// loader lock. When CTOR_FAULT_HELPER_FAULT is set in the process
// environment, the constructor performs a null-pointer store and the
// resulting EXCEPTION_ACCESS_VIOLATION must be observed by the libc master
// VEH that the LDR_DLL_NOTIFICATION_REASON_LOADED callback re-registered at
// the front of the VEH chain immediately before this ctor was invoked.
//
// Marked `volatile` so the optimizer cannot prove the store is dead and
// elide it (the whole point of the test).
struct CtorFault {
  CtorFault() {
    if (env_var_is_set("CTOR_FAULT_HELPER_FAULT")) {
      *static_cast<volatile int *>(nullptr) = 0xC70F;
    }
  }
};

CtorFault g_ctor_fault;

} // namespace

// Sentinel exported symbol. The parent test calls dlsym() for this symbol
// after a successful (non-faulting) dlopen() to verify the DLL actually
// loaded — distinguishing "ctor faulted but was caught" from "DLL never
// got loaded for some other reason".
extern "C" HELPER_EXPORT int ctor_fault_helper_loaded(void) { return 0xCAFE; }

// Bare DllMain — no per-process state, no CRT. Returning TRUE
// unconditionally is sufficient for a freestanding DLL.
extern "C" int DllMain(void * /*hinst*/, unsigned long /*reason*/,
                                 void * /*reserved*/) {
  return 1;
}
