//===-- PTY lpReserved2 extension --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_PTY_RESERVED2_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_PTY_RESERVED2_H

#include "hdr/stdint_proxy.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

inline constexpr uint32_t LLVM_LIBC_PTY_RESERVED2_MAGIC = 0x59545250; // 'PRTY'
inline constexpr uint16_t LLVM_LIBC_PTY_RESERVED2_VERSION = 3;
inline constexpr uint16_t LLVM_LIBC_PTY_ATTACH_STDIN = 0x0001;
inline constexpr uint16_t LLVM_LIBC_PTY_ATTACH_STDOUT = 0x0002;
inline constexpr uint16_t LLVM_LIBC_PTY_ATTACH_STDERR = 0x0004;
inline constexpr uint16_t LLVM_LIBC_PTY_ATTACH_ALL =
    LLVM_LIBC_PTY_ATTACH_STDIN | LLVM_LIBC_PTY_ATTACH_STDOUT |
    LLVM_LIBC_PTY_ATTACH_STDERR;

struct PtyReserved2Ext {
  uint32_t magic;
  uint16_t version;
  uint16_t flags;
  uint8_t tree_nonce[16];
  uint32_t attached_pty_id;
  uint32_t reserved0;
  HANDLE attached_pty_reference;
  HANDLE attached_pty_state_lock;
  HANDLE attached_pty_state_section;
};

static_assert(sizeof(PtyReserved2Ext) == 56,
              "PtyReserved2Ext is a cross-process ABI; do not change layout");

inline constexpr SIZE_T PTY_RESERVED2_EXT_SIZE = sizeof(PtyReserved2Ext);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_PTY_RESERVED2_H
