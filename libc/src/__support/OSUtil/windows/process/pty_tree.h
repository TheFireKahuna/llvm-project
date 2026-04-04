//===-- Tree-scoped PTY namespace and join service --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_PTY_TREE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_PTY_TREE_H

#include "hdr/stdint_proxy.h"
#include "hdr/types/pid_t.h"
#include "hdr/types/struct_winsize.h"
#include "include/llvm-libc-macros/termios-macros.h"
#include "include/llvm-libc-types/struct_termios.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

struct PtySharedState;

namespace pty_tree {

struct OwnedHandles {
  HANDLE reference = nullptr;
  HANDLE connection = nullptr;
  HANDLE input = nullptr;
  HANDLE output = nullptr;
  HANDLE error = nullptr;
  HANDLE stdout_pipe = nullptr;
  HANDLE state_lock = nullptr;
  HANDLE state_section = nullptr;
};

struct CurrentAttachment {
  uint32_t pty_id = 0;
  HANDLE state_lock = nullptr;
  HANDLE state_section = nullptr;
};

void init();
void fini();
void fork_reinit();

void copy_tree_nonce(uint8_t (&out)[16]);
bool has_tree_nonce();
HANDLE take_inherited_reference();
uint16_t inherited_attach_flags();

ErrorOr<uint32_t> allocate_pty_id();

ErrorOr<int> create_shared_state(uint32_t id, HANDLE *state_lock,
                                 HANDLE *state_section,
                                 PtySharedState **state_view);
int duplicate_state_handles(HANDLE source_lock, HANDLE source_section,
                            HANDLE *state_lock, HANDLE *state_section,
                            bool inherit = false);
int map_shared_state_view(HANDLE state_section, uint32_t expected_id,
                          PtySharedState **state_view);
void close_shared_state(HANDLE *state_lock, HANDLE *state_section,
                        PtySharedState **state_view);

ErrorOr<int> register_owned_pty(uint32_t id, const OwnedHandles &handles);
void unregister_owned_pty(uint32_t id);
ErrorOr<OwnedHandles> duplicate_local_owned_handles(uint32_t id);
ErrorOr<OwnedHandles> join_handles(uint32_t id);

uint32_t current_attached_pty_id();
bool has_current_attached_pty();
// Raw atomic read — bypasses both pty_tree::init() and the vt_pty
// adoption gate that current_attached_pty_id() triggers. Only callers
// running inside vt_pty's adoption InitFn should use this; user code
// must go through current_attached_pty_id() so adoption is ensured.
uint32_t current_attached_pty_id_unchecked();
int duplicate_current_attachment(CurrentAttachment *attachment,
                                 bool inherit = false);
void release_current_attachment(CurrentAttachment *attachment);
int set_current_attached_pty(const CurrentAttachment &attachment);
void clear_current_attached_pty();

int current_get_attr(struct termios *attrs);
int current_set_attr(const struct termios *attrs);
int set_attr(uint32_t pty_id, HANDLE state_section, const struct termios *attrs);
int seed_initial_session(HANDLE state_lock, HANDLE state_section, pid_t sid,
                         pid_t pgid);
int set_session_controller(uint32_t pty_id, HANDLE state_section, pid_t sid,
                           pid_t pgid, pid_t controller_pid,
                           uint64_t controller_create_time);
int current_get_sid(pid_t *sid);
int current_get_foreground_pgrp(pid_t *pgid);
int current_set_foreground_pgrp(pid_t pgid);
int set_foreground_pgrp(uint32_t pty_id, HANDLE state_section, pid_t pgid);
int current_get_winsize(struct winsize *ws);

// Coalesced seqlock snapshot of all fields needed by sync_attached_pty_state.
// Zero syscalls on the hot path — reads the PAGE_READONLY shared memory view
// under the seqlock with retry on torn reads.
struct SyncSnapshot {
  struct termios attrs;
  pid_t controlling_sid;
  pid_t foreground_pgrp;
};
int current_get_sync_snapshot(SyncSnapshot *snapshot);

} // namespace pty_tree
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_PTY_TREE_H
