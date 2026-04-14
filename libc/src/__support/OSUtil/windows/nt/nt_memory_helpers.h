//===-- Typed NT memory operation helpers ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Small, dependency-free wrappers around raw NT memory syscalls. These
// eliminate repeated LARGE_INTEGER/SIZE_T boilerplate and prevent typos
// in flag/struct setup across subsystems.
//
// Depends only on nt_memory_api.h and nt_types.h — safe to include from
// early-init code (tls, signal, process) without pulling in the memory
// subsystem, alloc layer, or any libc internals.
//
//   query_basic_info    — NtQueryVirtualMemory(MemoryBasicInformation)
//   extend_section      — NtExtendSection with SIZE_T interface
//   cfg_register_target — NtSetInformationVirtualMemory(VmCfgCallTargetInformation)
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_MEMORY_HELPERS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_MEMORY_HELPERS_H

#include "src/__support/OSUtil/windows/nt/nt_memory_api.h"
#include "src/__support/OSUtil/windows/nt/nt_memory_types.h"
#include "src/__support/OSUtil/windows/nt/nt_process_types.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace nt_helpers {

//===----------------------------------------------------------------------===//
// Query
//===----------------------------------------------------------------------===//

/// Query MEMORY_BASIC_INFORMATION for a single address.
/// Returns the raw NTSTATUS (caller decides success/failure semantics).
LIBC_INLINE NTSTATUS query_basic_info(const void *addr,
                                      MEMORY_BASIC_INFORMATION &mbi) {
  SIZE_T ret;
  return ::NtQueryVirtualMemory(NtCurrentProcess(), const_cast<PVOID>(addr),
                                MemoryBasicInformation, &mbi, sizeof(mbi),
                                &ret);
}

//===----------------------------------------------------------------------===//
// Section
//===----------------------------------------------------------------------===//

/// Extend a section's maximum size. Accepts a raw HANDLE and SIZE_T,
/// handling the LARGE_INTEGER conversion internally.
/// Returns raw NTSTATUS for caller-specific error handling.
LIBC_INLINE NTSTATUS extend_section(HANDLE section, SIZE_T new_max_size) {
  LARGE_INTEGER new_max;
  new_max.QuadPart = static_cast<LONGLONG>(new_max_size);
  return ::NtExtendSection(section, &new_max);
}

//===----------------------------------------------------------------------===//
// CFG (Control Flow Guard)
//===----------------------------------------------------------------------===//

/// Register a function address as a valid CFG indirect-call target.
/// Required before the address can be used as a thread start or indirect
/// call target on CFG-enabled processes.
LIBC_INLINE NTSTATUS cfg_register_target(void *func) {
  auto addr = reinterpret_cast<ULONG_PTR>(func);

  ULONG_PTR page_base = addr & ~static_cast<ULONG_PTR>(0xFFF);
  ULONG_PTR page_offset = addr - page_base;

  CFG_CALL_TARGET_INFO target = {};
  target.Offset = page_offset;
  target.Flags = CFG_CALL_TARGET_VALID;

  ULONG processed = 0;
  CFG_CALL_TARGET_LIST_INFORMATION info = {};
  info.NumberOfEntries = 1;
  info.NumberOfEntriesProcessed = &processed;
  info.CallTargetInfo = &target;

  MEMORY_RANGE_ENTRY range = {};
  range.VirtualAddress = reinterpret_cast<PVOID>(page_base);
  range.NumberOfBytes = 0x1000;

  return ::NtSetInformationVirtualMemory(NtCurrentProcess(),
                                         VmCfgCallTargetInformation, 1, &range,
                                         &info, sizeof(info));
}

} // namespace nt_helpers
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_MEMORY_HELPERS_H
