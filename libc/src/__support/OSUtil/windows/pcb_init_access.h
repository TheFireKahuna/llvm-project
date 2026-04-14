//===-- PcbInitAccess — Zone 0 write gate ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// PcbInitAccess is the sole class that can write PcbZone0 private fields.
// It is friended by PcbZone0 and provides static setter methods.
//
// Include this header ONLY in:
//   - startup init functions (app_init.cpp, process_identity.cpp)
//   - fork reinit functions (process_control_block.cpp)
//
// Subsystem code that only reads Zone 0 should use the public const
// accessors on PcbZone0 (g_pcb.zone0.page_size(), etc.) and should
// NOT include this header.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PCB_INIT_ACCESS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PCB_INIT_ACCESS_H

#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

class PcbInitAccess {
public:
  // --- System info (pcb_startup_init, Phase 0) ---

  LIBC_INLINE static void set_page_size(uint32_t v) {
    g_pcb.zone0.page_size_ = v;
  }
  LIBC_INLINE static void set_alloc_granularity(uint32_t v) {
    g_pcb.zone0.alloc_granularity_ = v;
  }
  LIBC_INLINE static void set_min_address(void *v) {
    g_pcb.zone0.min_address_ = v;
  }
  LIBC_INLINE static void set_max_address(void *v) {
    g_pcb.zone0.max_address_ = v;
  }

  // --- Module identity (pcb_startup_init, Phase 0) ---

  LIBC_INLINE static void set_module_handle(void *v) {
    g_pcb.zone0.module_handle_ = v;
  }
  LIBC_INLINE static void set_dso_handle(void *v) {
    g_pcb.zone0.dso_handle_ = v;
  }

  // --- Security cookie (pcb_startup_init, Phase 0) ---

  LIBC_INLINE static void set_security_cookie(uintptr_t v) {
    g_pcb.zone0.security_cookie_ = v;
  }
  LIBC_INLINE static void set_security_cookie_complement(uintptr_t v) {
    g_pcb.zone0.security_cookie_complement_ = v;
  }

  // --- Process identity (pcb_startup_init + fork reinit) ---

  LIBC_INLINE static void set_pid(pid_t v) { g_pcb.zone0.pid_ = v; }
  LIBC_INLINE static void set_parent_pid(DWORD v) {
    g_pcb.zone0.parent_pid_ = v;
  }

  // --- Canary (pcb_startup_init, Phase 0) ---

  LIBC_INLINE static void init_canary() {
    g_pcb.zone0.zone_canary_ =
        g_pcb.zone0.security_cookie_ ^ PCB_CANARY_MAGIC;
  }

  // --- NT capabilities (pcb_startup_init, Phase 0) ---

  LIBC_INLINE static void set_nt_build(uint32_t v) {
    g_pcb.zone0.nt_build_ = v;
  }
  LIBC_INLINE static void set_capabilities(uint32_t v) {
    g_pcb.zone0.capabilities_ = v;
  }
  LIBC_INLINE static NtOptionalSyscalls &optional_mut() {
    return g_pcb.zone0.optional_;
  }
};

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PCB_INIT_ACCESS_H
