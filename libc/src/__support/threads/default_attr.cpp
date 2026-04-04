//===-- Process-wide default thread attributes ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/threads/default_attr.h"

#include "src/__support/macros/config.h"
#include "src/__support/threads/spin_lock.h"
#include "src/__support/threads/thread.h" // For Thread::DEFAULT_*

namespace LIBC_NAMESPACE_DECL {
namespace internal {

namespace {

// SpinLock has a constexpr ctor and is therefore static-init safe — usable
// before any user code runs. Contention is effectively nil: the only writer
// is pthread_setattr_default_np, called rarely and typically once during
// process startup.
SpinLock g_default_attr_lock;

size_t g_default_stacksize = Thread::DEFAULT_STACKSIZE;
size_t g_default_guardsize = Thread::DEFAULT_GUARDSIZE;

class ScopedSpin {
  SpinLock &lk;

public:
  explicit ScopedSpin(SpinLock &l) : lk(l) { lk.lock(); }
  ~ScopedSpin() { lk.unlock(); }
};

} // namespace

size_t get_default_stacksize() {
  ScopedSpin guard(g_default_attr_lock);
  return g_default_stacksize;
}

size_t get_default_guardsize() {
  ScopedSpin guard(g_default_attr_lock);
  return g_default_guardsize;
}

size_t bump_default_stacksize(size_t size) {
  ScopedSpin guard(g_default_attr_lock);
  if (size > g_default_stacksize)
    g_default_stacksize = size;
  return g_default_stacksize;
}

size_t bump_default_guardsize(size_t size) {
  ScopedSpin guard(g_default_attr_lock);
  if (size > g_default_guardsize)
    g_default_guardsize = size;
  return g_default_guardsize;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
