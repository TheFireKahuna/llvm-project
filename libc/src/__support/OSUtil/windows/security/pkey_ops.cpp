//===---------- Windows pkey operations engine -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pkey_ops.h"

#include "hdr/errno_macros.h"
#include "src/__support/macros/config.h"

#ifdef LIBC_TARGET_ARCH_IS_X86_64
#include "src/__support/OSUtil/windows/security/pkey_state.h"
#endif

#include "src/__support/OSUtil/windows/libc_subsystem_init.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t pkey_alloc(unsigned int flags, unsigned int access_rights) {
#ifndef LIBC_TARGET_ARCH_IS_X86_64
  (void)flags;
  (void)access_rights;
  return -ENOSYS;
#else
  if (flags != 0)
    return -EINVAL;

  if (access_rights > windows::PKEY_MASK)
    return -EINVAL;

  int key = windows::pkey_alloc_key(access_rights);
  if (key < 0)
    return -ENOSPC;

  return key;
#endif
}

intptr_t pkey_free(int pkey) {
#ifndef LIBC_TARGET_ARCH_IS_X86_64
  (void)pkey;
  return -ENOSYS;
#else
  if (!windows::pkey_free_key(pkey))
    return -EINVAL;
  return 0;
#endif
}

intptr_t pkey_get(int pkey) {
#ifndef LIBC_TARGET_ARCH_IS_X86_64
  (void)pkey;
  return -ENOSYS;
#else
  int result = windows::pkey_get_rights(pkey);
  if (result < 0)
    return -EINVAL;
  return result;
#endif
}

intptr_t pkey_set(int pkey, unsigned int access_rights) {
#ifndef LIBC_TARGET_ARCH_IS_X86_64
  (void)pkey;
  (void)access_rights;
  return -ENOSYS;
#else
  if (!windows::pkey_set_rights(pkey, access_rights))
    return -EINVAL;
  return 0;
#endif
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#ifdef LIBC_TARGET_ARCH_IS_X86_64
void LIBC_NAMESPACE::internal::pkey_fork_reinit() {
  LIBC_NAMESPACE::windows::pkey_fork_reinit();
}
#else
void LIBC_NAMESPACE::internal::pkey_fork_reinit() {}
#endif
