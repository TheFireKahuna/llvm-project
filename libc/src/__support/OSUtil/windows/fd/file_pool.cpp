//===-- FILE slot pool implementation -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/fd/file_pool.h"
#include "src/__support/OSUtil/windows/alloc/slab_pool.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace file_pool {

static SlabPool pool;

void init() {
  pool.init(SLOT_SIZE);
  pool.init_tls();
}

void *alloc() { return pool.tls_alloc(); }

void free(void *slot) { SlabPool::free(slot); }
void fork_reinit() { pool.fork_reinit(); }

} // namespace file_pool
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace file_pool {
void destroy() { pool.destroy(); }
} // namespace file_pool
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

int LIBC_NAMESPACE::internal::file_pool_startup_init() {
  LIBC_NAMESPACE::internal::file_pool::init();
  return 0;
}

void LIBC_NAMESPACE::internal::file_pool_startup_fini() {
  LIBC_NAMESPACE::internal::file_pool::destroy();
}

void LIBC_NAMESPACE::internal::file_pool_fork_reinit() {
  LIBC_NAMESPACE::internal::file_pool::fork_reinit();
}
