//===-- Shared IOCP completion router multiplexer ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Multiplexing layer atop reactor::set_completion_router. The reactor only
// supports a single installed router, but several subsystems (epoll, socket
// I/O, future IPC transports) need to receive non-reactor completions. This
// module installs itself once as the reactor's sole router and dispatches to
// per-subsystem handlers based on the completion key.
//
// Two registration kinds:
//
//   install_sentinel_handler(key, h)
//     Pointer-identity dispatch. When the multiplexer sees a completion with
//     KeyContext == key, it calls h. Used by subsystems that stamp every
//     one of their IRPs with a fixed singleton key (e.g., socket I/O).
//
//   install_default_handler(h)
//     Catch-all. Invoked for completions that match no registered sentinel.
//     Used by subsystems that encode per-registration state in the key
//     pointer itself (e.g., epoll, where KeyContext is an
//     EpollRegistration*).
//
// Invariants:
//   - Registration is init-time only (Tier B Phase 7 or lazy-on-first-use).
//     The sentinel table is append-only with a release-published count,
//     so dispatch reads are lock-free and safe concurrent with registration.
//   - At most one default handler. Last writer wins (epoll's current
//     ensure_router_installed is idempotent, so this is benign).
//   - The multiplexer installs itself into the reactor on first call to
//     either install_* function, via reactor::set_completion_router. It is
//     safe to register in any order relative to reactor_startup_init as
//     long as reactor::iocp_handle() is non-null before the first
//     completion fires.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_REACTOR_COMPLETION_ROUTER_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_REACTOR_COMPLETION_ROUTER_H

#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace completion_router {

/// Subsystem handler signature. Mirrors reactor::CompletionRouter.
using Handler = void (*)(PVOID key, PVOID apc_context, NTSTATUS status,
                         ULONG_PTR information);

/// Register a handler that fires when KeyContext == @p sentinel exactly.
/// @p sentinel must be a stable singleton pointer (lifetime = process).
/// Idempotent: calling twice with the same sentinel+handler is a no-op.
void install_sentinel_handler(PVOID sentinel, Handler h);

/// Register the catch-all handler. Invoked for any completion whose key
/// does not match a registered sentinel. Last writer wins.
void install_default_handler(Handler h);

} // namespace completion_router
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_REACTOR_COMPLETION_ROUTER_H
