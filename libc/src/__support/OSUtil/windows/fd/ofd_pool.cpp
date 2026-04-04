//===-- Open file description pool implementation -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/fd/ofd_pool.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/alloc/slab_pool.h"
#include "src/__support/OSUtil/windows/libc_fini_registry.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/macros/config.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

static_assert(sizeof(LIBC_NAMESPACE::internal::OpenFileDescription) <=
                  LIBC_NAMESPACE::internal::ofd_pool::SLOT_SIZE,
              "OpenFileDescription exceeds OFD pool slot size");

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace ofd_pool {

static SlabPool pool;

void init() {
  pool.init(SLOT_SIZE);
  pool.init_tls();
}

OpenFileDescription *alloc() {
  void *slot = pool.tls_alloc();
  if (slot)
    // Defense-in-depth: slabs zero on free, but OFDs hold security-sensitive
    // state (handles, access flags). Explicit zero guards against any future
    // slab-recycling path that skips zeroing.
    __builtin_memset(slot, 0, SLOT_SIZE);
  return static_cast<OpenFileDescription *>(slot);
}

void free(OpenFileDescription *ofd) { SlabPool::free(ofd); }

void fork_reinit() { pool.fork_reinit(); }

} // namespace ofd_pool
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace ofd_pool {
void destroy() { pool.destroy(); }
} // namespace ofd_pool
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

int LIBC_NAMESPACE::internal::ofd_pool_startup_init() {
  LIBC_NAMESPACE::internal::ofd_pool::init();
  return 0;
}

void LIBC_NAMESPACE::internal::ofd_pool_fork_reinit() {
  LIBC_NAMESPACE::internal::ofd_pool::fork_reinit();
}

LIBC_REGISTER_FINI(4, ofd_pool,
                   &::LIBC_NAMESPACE::internal::ofd_pool::destroy)

LIBC_REGISTER_FORK_REINIT(ofd_pool,
                          ::LIBC_NAMESPACE::internal::kForkPrioOfdPool,
                          &::LIBC_NAMESPACE::internal::ofd_pool_fork_reinit)
