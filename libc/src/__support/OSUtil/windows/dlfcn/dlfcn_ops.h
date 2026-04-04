//===-- Internal dlfcn engine declarations -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Forward declarations for internal:: dlfcn functions that implement POSIX
// dlopen/dlsym/dlclose/dladdr/dlinfo semantics on Windows NT.
//
// Return convention: 0/-errno for long, value/-errno for intptr_t.
// dlerror has no engine — it is pure entry-point-layer error state.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_DLFCN_DLFCN_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_DLFCN_DLFCN_OPS_H

#include "hdr/types/dl_info.h"
#include "src/__support/macros/config.h"
#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Returns handle as intptr_t (>= 0) on success, -errno on failure.
intptr_t dlopen(const char *path, int mode);

// Returns symbol address as intptr_t (>= 0) on success, -errno on failure.
// Note: dlsym can legitimately return address 0 for some symbols, but
// on Windows with ASLR this is practically impossible for loaded modules.
intptr_t dlsym(void *__restrict handle, const char *__restrict symbol);

// Returns 0 on success, -errno on failure.
intptr_t dlclose(void *handle);

// Returns 1 if address was found, 0 if not. No error code (dladdr has no
// error reporting mechanism per POSIX).
intptr_t dladdr(const void *__restrict addr, Dl_info *__restrict info);

// Returns 0 on success, -errno on failure.
intptr_t dlinfo(void *__restrict handle, int request, void *__restrict info);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_DLFCN_DLFCN_OPS_H
