//===-- Global slab registry definition ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Single definition of the global SlabRegistry shared by all SlabPool
// instances. Zero-initialized (BSS). L1 directory is demand-committed
// VA; L2 bitmaps are page_alloc'd on first use per 4GB region. No
// growth copies, no leaks.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/alloc/slab_pool.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

SlabRegistry slab_registry = {};

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
