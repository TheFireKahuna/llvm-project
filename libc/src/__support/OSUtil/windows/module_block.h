//===-- Per-image libc teardown descriptor for DLL unload ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A libc-linked DLL (not c.dll itself) that wants its own `.libcfin` /
// `.libclzr` swept on FreeLibrary exports a single
// `__libc_module_block` via LIBC_DEFINE_MODULE_BLOCK(). The main
// dll_notify_callback resolves the block with LdrGetProcedureAddress
// and walks the DLL's section ranges directly — no per-image walker
// function pointer, no exported C++ symbol whose linkage would fight
// with c.dll's own copy.
//
// Why c.dll does NOT adopt this macro:
//
//   c.dll's own FreeLibrary drives DllMain(DLL_PROCESS_DETACH) →
//   __libc_dll_fini() → `.libcfin` walker. The loader then fires
//   LDR_DLL_NOTIFICATION_REASON_UNLOADED, but by that point
//   veh_core_fini (phase 0) has already called
//   LdrUnregisterDllNotification. The callback is gone, so c.dll's
//   module block — if one existed — would never be read.
//
// What the consuming DLL must provide in the SAME TU as
// LIBC_DEFINE_MODULE_BLOCK:
//
//   LIBC_DEFINE_SECTION_BOOKENDS(libcfin,
//                                ::LIBC_NAMESPACE::internal::FiniEntry)
//   LIBC_DEFINE_SECTION_BOOKENDS(libclzr,
//                                ::LIBC_NAMESPACE::internal::LazyInitResetEntry)
//   LIBC_DEFINE_MODULE_BLOCK(__dso_handle)
//
// The bookends are static in their defining TU (by
// LIBC_DEFINE_SECTION_BOOKENDS design), so the module-block
// initializer must live next to them.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MODULE_BLOCK_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MODULE_BLOCK_H

#include "include/__llvm-libc-common.h"
#include "hdr/stdint_proxy.h"
#include "src/__support/OSUtil/windows/lazy_init_reset.h"
#include "src/__support/OSUtil/windows/libc_fini_registry.h"
#include "src/__support/OSUtil/windows/nt/nt_string_api.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// ABI version for LibcModuleBlock. Bumped on incompatible field
// changes — the resolver rejects mismatched versions rather than walk
// a block laid out for a different libc.
inline constexpr uint32_t LIBC_MODULE_BLOCK_VERSION_V1 = 1;

// Per-image teardown descriptor. The notify callback reads this by
// name (`__libc_module_block`) from a departing DLL and walks the
// advertised section ranges. Entry types are POD-stable — they cross
// the image boundary by value-layout, not by SBO'd vtable.
//
// Both sections use the same $A..$M/$P*..$Z layout as c.dll's own
// bookends; the walker reverses libcfin for phase-reverse teardown
// and walks libclzr forward. Both ranges are half-open [start, end);
// the $A sentinel at +0 is skipped by SectionRegistry::begin().
struct LibcModuleBlock {
  uint32_t version;
  uint32_t reserved;
  void *dso_handle;
  const FiniEntry *libcfin_start;
  const FiniEntry *libcfin_end;
  const LazyInitResetEntry *libclzr_start;
  const LazyInitResetEntry *libclzr_end;
};

// Resolve the `__libc_module_block` export of the DLL loaded at
// `dll_base`. Returns nullptr when the DLL is not libc-linked (no
// export) or advertises an unsupported version. Called inside the
// notify callback under loader lock: LdrGetProcedureAddress reads
// the PE export directory of a fully-mapped image, which is safe
// during LDR_DLL_NOTIFICATION_REASON_UNLOADED (the image is still
// mapped; only DLL_PROCESS_DETACH has returned).
LIBC_INLINE const LibcModuleBlock *resolve_module_block(void *dll_base) {
  if (!dll_base)
    return nullptr;
  ANSI_STRING proc_name;
  ::RtlInitString(&proc_name, "__libc_module_block");
  PVOID addr = nullptr;
  NTSTATUS st =
      ::LdrGetProcedureAddress(dll_base, &proc_name, 0, &addr);
  if (!NT_SUCCESS(st) || !addr)
    return nullptr;
  const auto *block = static_cast<const LibcModuleBlock *>(addr);
  if (block->version != LIBC_MODULE_BLOCK_VERSION_V1)
    return nullptr;
  return block;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

// Emit the `__libc_module_block` export for the current image. Must
// expand at namespace scope (file scope) in exactly one TU per
// adopting DLL. The TU must also contain the matching
// LIBC_DEFINE_SECTION_BOOKENDS(libcfin, ...) and
// LIBC_DEFINE_SECTION_BOOKENDS(libclzr, ...) — otherwise the bookend
// symbols the initializer references do not exist.
//
// `dso_handle_sym` is the image's own `__dso_handle` (or any stable
// per-DSO identifier registered against this image's
// __cxa_atexit entries).
#define LIBC_DEFINE_MODULE_BLOCK(dso_handle_sym)                               \
  extern "C" __LIBC_DLLEXPORT_ATTR                                             \
      const ::LIBC_NAMESPACE::internal::LibcModuleBlock                        \
          __libc_module_block = {                                              \
              ::LIBC_NAMESPACE::internal::LIBC_MODULE_BLOCK_VERSION_V1,        \
              0,                                                               \
              &(dso_handle_sym),                                               \
              ::LIBC_NAMESPACE::internal::libc_libcfin_section_start,          \
              ::LIBC_NAMESPACE::internal::libc_libcfin_section_end,            \
              ::LIBC_NAMESPACE::internal::libc_libclzr_section_start,          \
              ::LIBC_NAMESPACE::internal::libc_libclzr_section_end,            \
  };

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MODULE_BLOCK_H
