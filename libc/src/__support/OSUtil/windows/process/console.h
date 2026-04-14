//===-- Process-level console interface -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Process-facing Windows console support for NT-POSIX.
//
// This layer sits above the raw ConDrv transport in ipc/condrv.h and below
// higher-level signal / tty policy. It owns:
//   - process publication of active console handles
//   - async ctrl callback registration
//   - lifecycle composition for launch/attach of modern conhost sessions
//
// It intentionally does NOT re-expose the full packet surface from condrv.h.
// Transport stays there; process/session policy lives here.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_CONSOLE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_CONSOLE_H

#include "src/__support/OSUtil/windows/ipc/condrv.h"
#include "src/__support/OSUtil/windows/nt/nt_memory_helpers.h"
#include "src/__support/OSUtil/windows/process/terminal_ops.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace console {

inline constexpr DWORD CTRL_C_EVENT = 0;
inline constexpr DWORD CTRL_BREAK_EVENT = 1;
inline constexpr DWORD CTRL_CLOSE_EVENT = 2;
inline constexpr DWORD CTRL_LOGOFF_EVENT = 5;
inline constexpr DWORD CTRL_SHUTDOWN_EVENT = 6;
inline constexpr ULONG CSR_CONSOLE_SERVER_ID = 1;
inline constexpr ULONG CSR_CONSOLE_CTRL_ROUTINE_SIZE = sizeof(void *);

// Atomic RMW: the console host's injected ctrl-dispatch thread reads
// ConsoleFlags concurrently with sigaction() modifying it.
LIBC_INLINE void set_ctrl_c_ignore(bool ignore) {
  auto *params = NtCurrentPeb()->ProcessParameters;
  if (!params)
    return;
  if (ignore)
    __atomic_fetch_or(&params->ConsoleFlags, 1u, __ATOMIC_RELAXED);
  else
    __atomic_fetch_and(&params->ConsoleFlags, ~1u, __ATOMIC_RELAXED);
}

LIBC_INLINE bool get_ctrl_c_ignore() {
  auto *params = NtCurrentPeb()->ProcessParameters;
  if (!params)
    return false;
  return __atomic_load_n(&params->ConsoleFlags, __ATOMIC_RELAXED) & 1u;
}

LIBC_INLINE NTSTATUS cfg_register_call_target(void *func) {
  return nt_helpers::cfg_register_target(func);
}

LIBC_INLINE int ctrl_event_to_signal(DWORD event) {
  switch (event) {
  case CTRL_C_EVENT:
    return 2; // SIGINT
  case CTRL_BREAK_EVENT:
    return 3; // SIGQUIT
  case CTRL_CLOSE_EVENT:
  case CTRL_LOGOFF_EVENT:
    return 1; // SIGHUP
  case CTRL_SHUTDOWN_EVENT:
    return 15; // SIGTERM
  default:
    return 0;
  }
}

using CtrlDispatchFn = DWORD(WINAPI *)(ULONG_PTR raw_param);

// Thread entry point injected by the host for async ctrl delivery.
DWORD WINAPI ctrl_dispatch(ULONG_PTR raw_param);
HANDLE current_reference();
void set_current_reference(HANDLE reference);

LIBC_INLINE NTSTATUS register_ctrl(CtrlDispatchFn dispatch_fn) {
  // Register the dispatch function as a valid CFG call target. If this fails
  // under CFG enforcement, the host-injected ctrl thread will crash when
  // calling the function. Propagate the failure rather than proceeding with
  // an unregistered target.
  NTSTATUS cfg_status =
      cfg_register_call_target(reinterpret_cast<void *>(dispatch_fn));
  if (!NT_SUCCESS(cfg_status))
    return cfg_status;

  void *fn_ptr = reinterpret_cast<void *>(dispatch_fn);
  BOOLEAN server_to_server = 0;
  return ::CsrClientConnectToServer(nullptr, CSR_CONSOLE_SERVER_ID, &fn_ptr,
                                    CSR_CONSOLE_CTRL_ROUTINE_SIZE,
                                    &server_to_server);
}

LIBC_INLINE NTSTATUS disconnect() {
  auto *params = NtCurrentPeb()->ProcessParameters;
  if (!params || !params->ConsoleHandle)
    return STATUS_INVALID_HANDLE;

  NTSTATUS status = ::NtClose(params->ConsoleHandle);
  if (NT_SUCCESS(status)) {
    params->ConsoleHandle = nullptr;
    set_current_reference(nullptr);
    internal::terminal_ops::detach_controlling_terminal();
  }
  return status;
}

// PublishedState captures the PEB/KernelBase console state before
// publish_handles() modifies it, enabling rollback via restore_handles().
//
// Ownership contract:
//   - The "Published*" handles are DUPLICATES created by publish_handles().
//     They are owned by this struct and MUST be closed when restoring or
//     when the PublishedState is discarded without restore.
//   - The "Console*/Standard*" handles are SNAPSHOTS of pre-existing PEB
//     values — they are NOT owned by this struct (they belong to whoever
//     owned them before publish_handles was called).
//   - restore_handles() consumes the Published* handles (closes them) and
//     writes the snapshot values back to the PEB.
//
// Thread safety: publish_handles() and restore_handles() mutate PEB fields
// and the fd table without internal synchronization. Callers MUST ensure
// no concurrent access to PEB->ProcessParameters console/stdio handles
// while these operations are in flight. In practice this means:
//   - Single-threaded during process init (CRT startup)
//   - Externally serialized during attach/detach (hold session lock)
struct PublishedState {
  HANDLE console_handle = nullptr;
  HANDLE console_reference = nullptr;
  HANDLE standard_input = nullptr;
  HANDLE standard_output = nullptr;
  HANDLE standard_error = nullptr;
  HANDLE published_reference = nullptr;
  HANDLE published_console_handle = nullptr;
  HANDLE published_standard_input = nullptr;
  HANDLE published_standard_output = nullptr;
  HANDLE published_standard_error = nullptr;
  ULONG_PTR console_host_cookie = 0;
  bool has_console_host_cookie = false;
};

LIBC_INLINE NTSTATUS query_console_host_cookie(ULONG_PTR *cookie) {
  if (!cookie)
    return STATUS_INVALID_PARAMETER;

  ULONG_PTR value = 0;
  ULONG return_length = 0;
  NTSTATUS status =
      ::NtQueryInformationProcess(NtCurrentProcess(), ProcessConsoleHostProcess,
                                  &value, sizeof(value), &return_length);
  if (!NT_SUCCESS(status))
    return status;
  if (return_length != 0 && return_length != sizeof(value))
    return STATUS_INFO_LENGTH_MISMATCH;
  *cookie = value;
  return STATUS_SUCCESS;
}

LIBC_INLINE NTSTATUS set_console_host_cookie(ULONG_PTR cookie) {
  return ::NtSetInformationProcess(NtCurrentProcess(), ProcessConsoleHostProcess,
                                   &cookie, sizeof(cookie));
}

NTSTATUS sync_process_console_state();

// Publish ConDrv client handles to the process PEB (ConsoleHandle,
// StandardInput/Output/Error) and update the fd table.
//
// Research note: KERNELBASE appears to maintain its own private 7-word
// console-state cache behind BaseGetConsoleReference/GetConsoleReference().
// We intentionally do not pattern-scan or mutate that cache at runtime.
// The decoded behavior is retained only as pseudocode for future reference:
//
//   state[1] = connection_handle;
//   state[2] = console_reference;
//   state[3] = stdin_handle;
//   state[4] = stdout_handle;
//   state[5] = stderr_handle;
//   state[6] = console_host_cookie;
//
//   // KERNELBASE appears to treat state[1] as the publish/validity word.
//   // Any faithful reimplementation belongs in explicit libc-owned state,
//   // not in runtime mutation of foreign private storage.
//
// Thread safety: NOT thread-safe. Mutates PEB->ProcessParameters fields
// and calls fd_table.rebind_std_fds* without synchronization. Caller must
// ensure exclusive access — typically single-threaded at process init, or
// externally serialized under a session lock during attach/detach.
NTSTATUS publish_handles(const condrv::CONDRV_CLIENT_HANDLES &handles,
                         HANDLE reference = nullptr,
                         PublishedState *saved = nullptr);

// Restore previously captured PEB state and close the published handles.
//
// Thread safety: same contract as publish_handles — caller must serialize.
NTSTATUS restore_handles(PublishedState *saved);

// Configuration for console session launch/attach.
//
// Two mutually exclusive modes:
//
// 1. **InitialServerMessage != nullptr** (pre-built payload)
//    Short-circuits all other string fields (Title, ApplicationName,
//    CurrentDirectory) and server path fields (ServerImagePath,
//    ServerCommandLine). The provided CONSOLE_SERVER_MSG is used verbatim
//    as the conhost connection payload. The boolean flags
//    (InheritClientHandles, PublishToProcess, ConsoleApp, WindowVisible)
//    still apply to session lifecycle but do NOT affect the message content.
//
// 2. **InitialServerMessage == nullptr** (default, build from PEB)
//    build_server_message() seeds the payload from the current process's
//    RTL_USER_PROCESS_PARAMETERS, with optional per-field overrides from
//    Title, ApplicationName, and CurrentDirectory. ServerImagePath and
//    ServerCommandLine control the conhost binary path; when both are
//    nullptr the default System32\conhost.exe path is constructed.
struct SessionOptions {
  bool inherit_client_handles = false;
  bool publish_to_process = true;
  bool console_app = true;
  bool window_visible = true;
  const WCHAR *title = nullptr;
  const WCHAR *application_name = nullptr;
  const WCHAR *current_directory = nullptr;
  const condrv::CONSOLE_SERVER_MSG *initial_server_message = nullptr;
  const WCHAR *server_image_path = nullptr;
  const WCHAR *server_command_line = nullptr;
};

// Move-only RAII wrapper for a console session.
//
// Owns the Server, Reference, and client handles (Connection/Input/Output/
// Error). Destructor closes all owned handles and restores published state
// if the session was published to the PEB.
//
// Fields are public for direct access by vt_pty and other subsystems that
// build sessions incrementally. The destructor only fires for non-moved-from
// instances (moved-from state: Handles.Connection == nullptr).
struct Session {
  HANDLE server = nullptr;
  HANDLE reference = nullptr;
  condrv::CONDRV_CLIENT_HANDLES handles = {};
  PublishedState saved_published_state = {};
  bool published = false;

  LIBC_INLINE Session() = default;

  LIBC_INLINE ~Session() { (void)close_owned(); }

  // Move-only.
  LIBC_INLINE Session(Session &&o) noexcept
      : server(o.server), reference(o.reference), handles(o.handles),
        saved_published_state(o.saved_published_state),
        published(o.published) {
    o.server = nullptr;
    o.reference = nullptr;
    o.handles = {};
    o.saved_published_state = {};
    o.published = false;
  }

  LIBC_INLINE Session &operator=(Session &&o) noexcept {
    if (this != &o) {
      (void)close_owned();
      server = o.server;
      reference = o.reference;
      handles = o.handles;
      saved_published_state = o.saved_published_state;
      published = o.published;
      o.server = nullptr;
      o.reference = nullptr;
      o.handles = {};
      o.saved_published_state = {};
      o.published = false;
    }
    return *this;
  }

  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;

  // Release ownership of all handles without closing them. Returns the
  // current state and leaves this Session in a moved-from (empty) state.
  LIBC_INLINE Session release() noexcept {
    Session tmp;
    tmp.server = server;
    tmp.reference = reference;
    tmp.handles = handles;
    tmp.saved_published_state = saved_published_state;
    tmp.published = published;
    server = nullptr;
    reference = nullptr;
    handles = {};
    saved_published_state = {};
    published = false;
    return tmp;
  }

  LIBC_INLINE bool is_active() const { return handles.Connection != nullptr; }

  // Close all owned handles and restore published state. Safe to call on
  // an already-closed or moved-from session (no-op). Called by destructor
  // and by the console::close() free function.
  // Returns the NTSTATUS from restore_handles() if the session was published,
  // or STATUS_SUCCESS otherwise. Handles are always closed regardless of
  // whether restore succeeded.
  NTSTATUS close_owned();
};


NTSTATUS build_server_message(condrv::CONSOLE_SERVER_MSG *msg,
                              const SessionOptions &options);

NTSTATUS publish(Session *session);

NTSTATUS restore(Session *session);

// Close a composed console session. When deliver_sighup is set, emit a
// terminal-style SIGHUP exactly once for the published session teardown.
NTSTATUS close(Session *session, bool deliver_sighup = false);

// Launch a fresh modern console host using ConDrv directly. By default this
// builds the observed Windows 11 V2 path:
//   <NtSystemRoot>\System32\conhost.exe 0xffffffff
// without the legacy -ForceV1 switch.
NTSTATUS launch(Session *session, const SessionOptions &options = {});

// Attach to an existing console published by another process ID.
NTSTATUS attach(Session *session, HANDLE process_id,
                const SessionOptions &options = {});

LIBC_INLINE NTSTATUS generate_ctrl_event(DWORD event_type,
                                         DWORD process_group_id) {
  return condrv::generate_console_ctrl_event(event_type, process_group_id);
}

} // namespace console
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_CONSOLE_H
