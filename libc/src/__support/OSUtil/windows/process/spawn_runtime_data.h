//===-- Inherited spawn runtime-data access ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Native NtCreateUserProcess launch passes inherited fd/PTY/signal startup
// state through RTL_USER_PROCESS_PARAMETERS::RuntimeData. Our direct launcher
// also aliases that blob through ShellInfo so very early child startup can
// recover it before higher-level initialization finishes wiring the process.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_SPAWN_RUNTIME_DATA_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_SPAWN_RUNTIME_DATA_H

#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace process_utils {

struct InheritedRuntimeDataView {
  const BYTE *data = nullptr;
  SIZE_T size = 0;
};

LIBC_INLINE bool
get_inherited_runtime_data(InheritedRuntimeDataView *out) {
  if (!out)
    return false;

  *out = {};

  auto *params = NtCurrentPeb()->ProcessParameters;
  if (params && params->RuntimeData.Buffer &&
      params->RuntimeData.Length >= sizeof(DWORD)) {
    out->data = reinterpret_cast<const BYTE *>(params->RuntimeData.Buffer);
    out->size = static_cast<SIZE_T>(params->RuntimeData.Length);
    return true;
  }

  if (params && params->ShellInfo.Buffer &&
      params->ShellInfo.Length >= sizeof(DWORD)) {
    out->data = reinterpret_cast<const BYTE *>(params->ShellInfo.Buffer);
    out->size = static_cast<SIZE_T>(params->ShellInfo.Length);
    return true;
  }

  return false;
}

} // namespace process_utils
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_SPAWN_RUNTIME_DATA_H
