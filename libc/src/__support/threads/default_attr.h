//===-- Process-wide default thread attributes -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_DEFAULT_ATTR_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_DEFAULT_ATTR_H

#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Process-wide default thread stack/guard sizes consulted by
// pthread_attr_init. Mutated only via pthread_setattr_default_np, which
// matches musl's restriction to stacksize/guardsize — every other attr
// field stays a per-call decision.
//
// Reads happen on every pthread_attr_init / pthread_create-with-NULL-attr
// and are concurrent with writes from pthread_setattr_default_np. A
// SpinLock makes the get-then-store sequence atomic so a successful
// setattr_default_np is observed by subsequent attr_init calls.
//
// Fork inheritance is automatic: RtlCloneUserProcess (NTPOSIX) and
// clone() (Linux) both COW the parent's address space, so these statics
// arrive in the child with the parent's last-stored values.

size_t get_default_stacksize();
size_t get_default_guardsize();

// Max-only updates, matching musl's __default_stacksize semantics: a
// shorter request never lowers the stored value. Returns the post-update
// stored value (which may differ from `size` if a larger value was
// already in place).
size_t bump_default_stacksize(size_t size);
size_t bump_default_guardsize(size_t size);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_DEFAULT_ATTR_H
