//===-- cfguard.cpp - Control Flow Guard function pointers -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// CFG indirect call validation pointers. The Windows loader patches these
// with ntdll's validation routines at module init.
//
// `__guard_check_icall_fptr` is the validator (callee-cleanup C ABI: target in
// RCX, no transfer of control). A no-op return is correct fail-open behavior
// when CFG is not enforced — the caller's subsequent indirect call still runs.
//
// `__guard_dispatch_icall_fptr` and the XFG dispatch variants use a tail-jump
// ABI (target in RAX on x64, X15 on AArch64). The compiler emits
//     mov  rax, target
//     call [__guard_dispatch_icall_fptr]
// so a plain `ret` would silently skip the indirect call entirely. The
// fallbacks are naked tail-jumps that preserve forward progress when the
// loader hasn't patched the slot (CFG disabled at runtime, or pre-patch
// window). The fastfail path is intentionally avoided: it would convert
// CFG-disabled hosts into hard crashes for every indirect call.
//
// The .00cfg section is read-only after load, protecting these pointers
// from memory corruption. Each slot is `const volatile`: `const` drives
// clang to register the section as PSF_Read (no PSF_Write) via the
// Context.SectionInfos pipeline, which propagates to the PE section
// characteristics (IMAGE_SCN_MEM_READ, no IMAGE_SCN_MEM_WRITE);
// `volatile` prevents the compiler from caching stale values across
// the loader's patch (which has kernel privilege to write the pages
// before the image is transitioned to its final R/O protection).
//
//===----------------------------------------------------------------------===//

#include "include/__llvm-libc-common.h"
#include "src/__support/macros/config.h"

#include <stdint.h>

namespace {
// IMAGE_GUARD_* bits advertised in the load config so the loader actually
// enforces what we publish in load_config.cpp:
//   0x00000100  CF_INSTRUMENTED              — compiler emits __guard_check
//   0x00000400  CF_FUNCTION_TABLE_PRESENT    — GFID table is populated
//   0x00010000  CF_LONGJUMP_TABLE_PRESENT    — lld synthesizes when /guard:cf
//   0x00400000  EH_CONTINUATION_TABLE_PRESENT — driver passes -guard:ehcont
//   0x01000000  CASTGUARD_PRESENT            — failure handler pointer set
//
// XFG_ENABLED (0x00800000) is intentionally omitted: the GFID table layout
// must carry per-target hash bytes for the loader to validate XFG calls, and
// the toolchain driver does NOT emit -guard:xfg today. Advertising XFG with
// no hash bytes would cause the loader to reject every indirect call.
//
// MEMCPY_PRESENT (0x02000000) is omitted because GuardMemcpyFunctionPointer
// resolves to the empty fallback (load_config.cpp uses /alternatename to wire
// __guard_memcpy_fptr → __guard_memcpy_fptr_empty__).
constexpr unsigned long kGuardFlags =
    0x00000100UL |  // CF_INSTRUMENTED
    0x00000400UL |  // CF_FUNCTION_TABLE_PRESENT
    0x00010000UL |  // CF_LONGJUMP_TABLE_PRESENT
    0x00400000UL |  // EH_CONTINUATION_TABLE_PRESENT
    0x01000000UL;   // CASTGUARD_PRESENT
} // namespace

//===----------------------------------------------------------------------===//
// Pre-patch fallbacks
//
// `*_check_*` validators are no-op returns (allow-all behavior). Dispatch
// variants tail-jump to the indirect-call target so program semantics are
// preserved when the loader does not patch the .00cfg slot.
//===----------------------------------------------------------------------===//

#if defined(__x86_64__) || defined(__arm64ec__)
LIBC_MSABI [[clang::guard(nocf)]] static void guard_check_icall_nop(uintptr_t) {}

// x64 dispatch ABI: target in RAX. Naked tail-jump preserves forward progress.
LIBC_MSABI [[clang::guard(nocf)]] [[gnu::naked]] static void
guard_dispatch_icall_nop() {
  __asm__ volatile("jmp *%rax");
}

// XFG dispatch ABI matches CFG dispatch on x64 (target in RAX); the per-call
// XFG hash check is the only difference, which the no-op fallback skips.
LIBC_MSABI [[clang::guard(nocf)]] [[gnu::naked]] static void
guard_xfg_dispatch_icall_nop() {
  __asm__ volatile("jmp *%rax");
}
#elif defined(__aarch64__) && !defined(__arm64ec__)
[[clang::guard(nocf)]] static void guard_check_icall_nop() {}

// AArch64 dispatch ABI: target in X15. Naked tail-jump.
[[clang::guard(nocf)]] [[gnu::naked]] static void
guard_dispatch_icall_nop() {
  __asm__ volatile("br x15");
}
#elif defined(__arm__)
[[clang::guard(nocf)]] static void guard_check_icall_nop(uintptr_t) {}
#endif

//===----------------------------------------------------------------------===//
// CFG function pointers (placed in .00cfg for loader protection)
//
// Typed as function-pointer slots (not `void *`) so the initializers are
// bare function names — constant expressions. The slots are
// `const volatile`: `const` drives clang's type-derived section flags to
// PSF_Read only, which propagates to the emitted PE section
// characteristics (IMAGE_SCN_MEM_READ, no IMAGE_SCN_MEM_WRITE), keeping
// the MSVC CFG hardening contract that `.00cfg` is read-only at runtime;
// `volatile` prevents the compiler from caching stale values across the
// loader's ring-0 patch. No `#pragma section(".00cfg", read)` is needed
// — the variable types alone drive the section flags.
//
// At link time `.00cfg` is merged into `.rdata` — both lld-link and
// MSVC link.exe do this by default (the merge rule is built into the
// linker; see lld/COFF/Driver.cpp and the cited MSVC behavior). This is
// benign: the CFG contract is on load-config-directory RVAs, not section
// names, and `.rdata` is itself R/O in the final image, so the
// "read-only at runtime" guarantee holds. Expect no `.00cfg` in the
// section table of a linked binary — this matches MSVC output.
//===----------------------------------------------------------------------===//

// Derive pointer aliases from the pre-patch fallbacks declared above via
// `decltype`. LIBC_MSABI can legally decorate a function *declaration* but
// not a function *type* — the trailing-alias spelling
// (`using T = void() LIBC_MSABI;`) is rejected by GCC and warned by Clang
// under `-Wgcc-compat`. `decltype` of a decorated declaration lifts the
// attribute into the type system without that warning. CFG check/dispatch
// slots are hotpatched by ntdll's LdrpValidateUserCallTarget under MS x64
// ABI; the .00cfg storage must expose that ABI regardless of TU default.
#if defined(__x86_64__) || defined(__arm64ec__) || \
    (defined(__aarch64__) && !defined(__arm64ec__))
using CfgCheckFn = decltype(&guard_check_icall_nop);
using CfgDispatchFn = decltype(&guard_dispatch_icall_nop);
#elif defined(__arm__)
using CfgCheckFn = decltype(&guard_check_icall_nop);
// arm32 has no CFG dispatch fallback; the .00cfg slot stays nullptr until
// the loader patches it. Unreferenced prototype provides the pointer-type
// source. External linkage (no `static`) means `decltype` is unevaluated
// and nothing odr-uses the name, so the linker never sees a reference and
// drops the declaration silently — no symbol emitted.
void __cfg_dispatch_type_source();
using CfgDispatchFn = decltype(&__cfg_dispatch_type_source);
#endif

extern "C" {

#if defined(__x86_64__) || defined(__arm64ec__)
__LIBC_SECTION_ATTR(".00cfg") __LIBC_SELECTANY_ATTR extern const volatile
    CfgCheckFn __guard_check_icall_fptr = guard_check_icall_nop;
__LIBC_SECTION_ATTR(".00cfg") __LIBC_SELECTANY_ATTR extern const volatile
    CfgDispatchFn __guard_dispatch_icall_fptr = guard_dispatch_icall_nop;
#elif defined(__aarch64__) && !defined(__arm64ec__)
__LIBC_SECTION_ATTR(".00cfg") __LIBC_SELECTANY_ATTR extern const volatile
    CfgCheckFn __guard_check_icall_fptr = guard_check_icall_nop;
__LIBC_SECTION_ATTR(".00cfg") __LIBC_SELECTANY_ATTR extern const volatile
    CfgDispatchFn __guard_dispatch_icall_fptr = guard_dispatch_icall_nop;
#elif defined(__arm__)
__LIBC_SECTION_ATTR(".00cfg") __LIBC_SELECTANY_ATTR extern const volatile
    CfgCheckFn __guard_check_icall_fptr = guard_check_icall_nop;
__LIBC_SECTION_ATTR(".00cfg") __LIBC_SELECTANY_ATTR extern const volatile
    CfgDispatchFn __guard_dispatch_icall_fptr = nullptr;
#endif

__LIBC_SELECTANY_ATTR extern const unsigned long __guard_flags = kGuardFlags;

} // extern "C"

//===----------------------------------------------------------------------===//
// XFG (Extended Flow Guard) — Windows 11+
//===----------------------------------------------------------------------===//

extern "C" {

#if defined(__x86_64__) && !defined(__arm64ec__)
__LIBC_SECTION_ATTR(".00cfg") __LIBC_SELECTANY_ATTR extern const volatile
    CfgCheckFn __guard_xfg_check_icall_fptr = guard_check_icall_nop;
__LIBC_SECTION_ATTR(".00cfg") __LIBC_SELECTANY_ATTR extern const volatile
    CfgDispatchFn __guard_xfg_dispatch_icall_fptr = guard_xfg_dispatch_icall_nop;
__LIBC_SECTION_ATTR(".00cfg") __LIBC_SELECTANY_ATTR extern const volatile
    CfgDispatchFn __guard_xfg_table_dispatch_icall_fptr =
        guard_xfg_dispatch_icall_nop;
#else
__LIBC_SELECTANY_ATTR extern const volatile CfgCheckFn
    __guard_xfg_check_icall_fptr = nullptr;
__LIBC_SELECTANY_ATTR extern const volatile CfgDispatchFn
    __guard_xfg_dispatch_icall_fptr = nullptr;
__LIBC_SELECTANY_ATTR extern const volatile CfgDispatchFn
    __guard_xfg_table_dispatch_icall_fptr = nullptr;
#endif

} // extern "C"

//===----------------------------------------------------------------------===//
// CastGuard (Windows 11+) — vftable pointer validation
//===----------------------------------------------------------------------===//

// Compiler-emitted CastGuard failure dispatch uses MS x64 ABI. There is
// no pre-patch fallback (the slot stays nullptr until MSVC codegen wires
// the real handler), so an unreferenced MS-ABI prototype serves as the
// pointer-type source for `decltype` — unevaluated, no symbol emitted.
LIBC_MSABI void __castguard_type_source(void *);
using CastGuardHandler = decltype(&__castguard_type_source);

extern "C" {

__LIBC_SECTION_ATTR(".00cfg") __LIBC_SELECTANY_ATTR extern const volatile
    CastGuardHandler __castguard_check_failure_os_handled_fptr = nullptr;

} // extern "C"

//===----------------------------------------------------------------------===//
// ARM64EC interop pointers
//===----------------------------------------------------------------------===//

#if defined(__arm64ec__)
extern "C" {
__LIBC_SELECTANY_ATTR void *__os_arm64x_check_icall = nullptr;
__LIBC_SELECTANY_ATTR void *__os_arm64x_check_icall_cfg = nullptr;
__LIBC_SELECTANY_ATTR void *__os_arm64x_dispatch_call = nullptr;
__LIBC_SELECTANY_ATTR void *__os_arm64x_dispatch_call_no_redirect = nullptr;
}
#endif
