//===-- VT-backed PTY session support --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_VT_PTY_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_VT_PTY_H

#include "hdr/stdint_proxy.h"
#include "hdr/types/pid_t.h"
#include "hdr/types/size_t.h"
#include "hdr/types/ssize_t.h"
#include "hdr/types/struct_winsize.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process/terminal_foreground.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"

struct termios;

namespace LIBC_NAMESPACE_DECL {
namespace internal {

struct OpenFileDescription;

namespace vt_pty {

struct Session;
struct SpawnHandles {
  HANDLE reference = nullptr;
  HANDLE connection = nullptr;
  HANDLE input = nullptr;
  HANDLE output = nullptr;
  HANDLE error = nullptr;
  HANDLE stdout_pipe = nullptr;
  HANDLE state_lock = nullptr;
  HANDLE state_section = nullptr;
  uint32_t pty_id = 0;
  uintptr_t session_key = 0;
};

// First-use gate for inherited attached-PTY adoption. Callers through
// pty_tree::current_attached_pty_id / has_current_attached_pty trigger
// this implicitly; explicit calls are rare (pty_tree.cpp is the only
// current consumer, via a forward declaration).
void ensure_adoption();

bool is_pty(const OpenFileDescription *ofd);
bool is_master(const OpenFileDescription *ofd);
bool is_slave(const OpenFileDescription *ofd);
uintptr_t session_key(const OpenFileDescription *ofd);
bool is_ptmx_path(const char *path);
bool is_pts_path(const char *path);

ErrorOr<int> duplicate_spawn_handles(const OpenFileDescription *ofd,
                                     SpawnHandles *handles);
void close_spawn_handles(SpawnHandles *handles);

void retain(Session *session);
void release(Session *session);
void release_opaque(void *session);
void clear_current_attachment_keepalive();

ErrorOr<int> posix_openpt(int flags);
int grantpt(int fd);
int unlockpt(int fd);
int ptsname_r(int fd, char *buffer, size_t size);
ErrorOr<int> open_pts_path(const char *path, int flags);
ErrorOr<int> open_pts_id(uint32_t id, int flags);
ErrorOr<int> openpty(int *master_fd, int *slave_fd, char *name,
                     const struct termios *termp,
                     const struct winsize *winp);
int login_tty(int fd);

int is_terminal_fd(int fd);

// Backend API — called by terminal_ops dispatch layer.
// These take a pre-validated Session* and skip argument validation.
int validate_pty_fd(int fd, OpenFileDescription **ofd_out,
                    Session **session_out);
int check_foreground_access(Session *session, TerminalAccessKind kind);
int pty_get_attr(Session *session, struct termios *t);
int pty_set_attr(Session *session, int actions, const struct termios *t);
int pty_flush(Session *session, int queue_selector);
int pty_drain(Session *session);
ErrorOr<pid_t> pty_get_sid(Session *session);
ErrorOr<pid_t> pty_get_foreground_pgrp(Session *session);
int pty_set_foreground_pgrp(Session *session, pid_t pgid);
int pty_flow(Session *session, int action);
int pty_send_break(Session *session);
int pty_get_pending_input_bytes(Session *session, int *count);
int pty_get_winsize(Session *session, struct winsize *ws);
int pty_set_winsize(Session *session, const struct winsize *ws);

ssize_t read(OpenFileDescription *ofd, void *buffer, size_t count);
ssize_t write(OpenFileDescription *ofd, const void *buffer, size_t count);

} // namespace vt_pty
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_VT_PTY_H
