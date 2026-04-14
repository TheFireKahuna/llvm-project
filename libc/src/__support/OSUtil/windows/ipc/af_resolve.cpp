//===-- Address family ops resolver ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Central registry for address family ops tables. Adding a new AF means:
//   1. Create af_<name>_ops.{h,cpp}
//   2. Add a case here
//   3. Update CMakeLists.txt
// No existing AF code is touched.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/ipc/af_ops.h"
#include "src/__support/OSUtil/windows/ipc/af_unix_ops.h"
#include "include/llvm-libc-macros/sys-socket-macros.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

const AddressFamilyOps *resolve_af_ops(int domain) {
  switch (domain) {
  case AF_UNIX:
    return &af_unix_ops;
  // Future: case AF_INET: case AF_INET6: return &af_inet_ops;
  default:
    return nullptr;
  }
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
