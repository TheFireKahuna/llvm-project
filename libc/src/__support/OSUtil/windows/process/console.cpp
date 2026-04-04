//===-- Process-level console interface -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/process/console.h"
#include "src/__support/OSUtil/windows/lazy_init.h"
#include "src/__support/OSUtil/windows/lazy_init_reset.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"

#include "src/__support/CPP/scope_guard.h"
#include "src/__support/CPP/span.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/nt/nt_context_types.h"
#include "src/__support/OSUtil/windows/nt/nt_string_api.h"
#include "src/__support/OSUtil/windows/nt/nt_wstringstream.h"
#include "src/__support/OSUtil/windows/nt/shared_user_data.h"
#include "src/__support/OSUtil/windows/process/console_handle_utils.h"
#include "src/__support/OSUtil/windows/process/console_tty.h"
#include "src/__support/OSUtil/windows/process/terminal_ops.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/signal/signal.h"

// First-use gate for syncing the process console host cookie from the
// PEB's ConsoleHandle. Queries NtDeviceIoControlFile to obtain the
// conhost server PID and stores it back via ProcessConsoleHostProcess.
// One syscall worth of work — skipped entirely if the process never
// touches the console.
//
// Registered in `.libclzr` so exec_self_hollow() clears the gate; the
// new image has its own PEB ConsoleHandle whose cookie must be re-synced
// on first console use.
//
// Kept at `LIBC_NAMESPACE::internal` scope (not anon) so
// LIBC_REGISTER_LAZY_RESET's thunk at the bottom of this TU can resolve
// the symbol via fully-qualified lookup. Declared here (before
// console::current_reference) so the .ensure() call there compiles.
namespace LIBC_NAMESPACE_DECL {
namespace internal {
static int console_init_impl() {
  (void)LIBC_NAMESPACE::console::sync_process_console_state();
  return 0;
}
LazyInit<&console_init_impl> g_console_init;
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

namespace LIBC_NAMESPACE_DECL {
namespace console {
namespace {

static constexpr DWORD CTRL_EVENT_TYPE_MASK = 0x7FFFFFFFu;

static constexpr WCHAR DEFAULT_CONSOLE_TITLE[] = u"Command Prompt";
static constexpr WCHAR DEFAULT_CONHOST_SUFFIX[] = u"\\System32\\conhost.exe";
static constexpr WCHAR DEFAULT_CONHOST_ARGUMENTS[] = u" 0xffffffff";
static constexpr WCHAR NT_PATH_PREFIX[] = u"\\??\\";
// Enough for "\??\" + KUSER_SHARED_DATA::NtSystemRoot[260] +
// "\System32\conhost.exe" + NUL.
static constexpr size_t DEFAULT_CONHOST_IMAGE_BUFFER_CAPACITY = 320;
static constexpr size_t DEFAULT_CONHOST_COMMAND_BUFFER_CAPACITY =
    DEFAULT_CONHOST_IMAGE_BUFFER_CAPACITY +
    sizeof(DEFAULT_CONHOST_ARGUMENTS) / sizeof(WCHAR);

DWORD current_process_id() {
  return NtCurrentProcessId();
}

// Named constant for the low bit in the ProcessConsoleHostProcess cookie.
// When set, it indicates an active conhost attachment; the upper bits carry
// the conhost server PID. Both ntdll and KernelBase use this encoding.
static constexpr ULONG_PTR CONSOLE_HOST_COOKIE_ACTIVE_BIT = 1ULL;

using internal::console_util::close_handle_if_valid;
using internal::console_util::is_live_console_handle;

NTSTATUS sync_process_console_host_cookie_from_peb() {
  auto *params = NtCurrentPeb()->ProcessParameters;
  if (!params || !is_live_console_handle(params->ConsoleHandle))
    return STATUS_SUCCESS;

  ULONG64 server_pid = 0;
  NTSTATUS status =
      condrv::get_condrv_server_pid(params->ConsoleHandle, &server_pid);
  if (!NT_SUCCESS(status))
    return status;

  return set_console_host_cookie(
      static_cast<ULONG_PTR>(server_pid | CONSOLE_HOST_COOKIE_ACTIVE_BIT));
}

NTSTATUS duplicate_process_handle(HANDLE source, HANDLE *target) {
  if (!target)
    return STATUS_INVALID_PARAMETER;
  *target = nullptr;
  if (!source)
    return STATUS_SUCCESS;
  return ::NtDuplicateObject(NtCurrentProcess(), source, NtCurrentProcess(),
                             target, 0, 0, DUPLICATE_SAME_ACCESS);
}

HANDLE load_current_console_reference() {
  uintptr_t value =
      g_pcb.console.current_reference.load(cpp::MemoryOrder::ACQUIRE);
  return reinterpret_cast<HANDLE>(value);
}

void store_current_console_reference(HANDLE reference) {
  g_pcb.console.current_reference.store(reinterpret_cast<uintptr_t>(reference),
                                        cpp::MemoryOrder::RELEASE);
}

using windows::nt_wstring_view;
using windows::WStringStream;

// copy_field: truncating copy from a NUL-terminated wide string into a
// fixed-size buffer. Silently truncates if the source exceeds capacity.
template <size_t N>
NTSTATUS copy_field(WCHAR (&dest)[N], USHORT *dest_length,
                                const WCHAR *src) {
  if (!dest_length)
    return STATUS_INVALID_PARAMETER;

  if (!src) {
    dest[0] = 0;
    *dest_length = 0;
    return STATUS_SUCCESS;
  }

  size_t len = 0;
  while (src[len] != 0 && len + 1 < N) {
    dest[len] = src[len];
    ++len;
  }

  dest[len] = 0;
  *dest_length = static_cast<USHORT>(len * sizeof(WCHAR));
  return STATUS_SUCCESS;
}

// copy_field: truncating copy from a UNICODE_STRING into a fixed-size buffer.
template <size_t N>
NTSTATUS copy_field(WCHAR (&dest)[N], USHORT *dest_length,
                                const UNICODE_STRING &src) {
  if (!dest_length)
    return STATUS_INVALID_PARAMETER;

  size_t src_len = src.Length / sizeof(WCHAR);
  if (!src.Buffer || src_len == 0) {
    dest[0] = 0;
    *dest_length = 0;
    return STATUS_SUCCESS;
  }

  size_t len = src_len;
  if (len + 1 > N)
    len = N - 1;

  for (size_t i = 0; i < len; ++i)
    dest[i] = src.Buffer[i];

  dest[len] = 0;
  *dest_length = static_cast<USHORT>(len * sizeof(WCHAR));
  return STATUS_SUCCESS;
}

// copy_field_with_fallback: tries override (cstr), then fallback
// (UNICODE_STRING), then empty_fallback (cstr). Always truncating.
template <size_t N>
NTSTATUS copy_field_with_fallback(
    WCHAR (&dest)[N], USHORT *dest_length, const WCHAR *override_value,
    const UNICODE_STRING &fallback, const WCHAR *empty_fallback = nullptr) {
  if (override_value)
    return copy_field(dest, dest_length, override_value);
  if (fallback.Buffer && fallback.Length != 0)
    return copy_field(dest, dest_length, fallback);
  return copy_field(dest, dest_length, empty_fallback);
}

NTSTATUS
build_default_conhost_strings(const SessionOptions &options,
                              UNICODE_STRING *image_path,
                              UNICODE_STRING *command_line, WCHAR *image_buffer,
                              size_t image_capacity, WCHAR *command_buffer,
                              size_t command_capacity) {
  if (!image_path || !command_line || !image_buffer || !command_buffer)
    return STATUS_INVALID_PARAMETER;

  if (options.server_command_line && !options.server_image_path)
    return STATUS_INVALID_PARAMETER;

  if (options.server_image_path) {
    NTSTATUS status =
        ::RtlInitUnicodeStringEx(image_path, options.server_image_path);
    if (!NT_SUCCESS(status))
      return status;
  } else {
    WStringStream ss(cpp::span<WCHAR>(image_buffer, image_capacity - 1));
    ss << NT_PATH_PREFIX
       << windows_util::shared_user_data()->NtSystemRoot
       << DEFAULT_CONHOST_SUFFIX;
    if (ss.overflow())
      return STATUS_BUFFER_TOO_SMALL;
    ss.null_terminate();

    NTSTATUS status = ::RtlInitUnicodeStringEx(image_path, image_buffer);
    if (!NT_SUCCESS(status))
      return status;
  }

  if (options.server_command_line) {
    return ::RtlInitUnicodeStringEx(command_line, options.server_command_line);
  }

  {
    WStringStream ss(cpp::span<WCHAR>(command_buffer, command_capacity - 1));
    ss << image_path->Buffer << DEFAULT_CONHOST_ARGUMENTS;
    if (ss.overflow())
      return STATUS_BUFFER_TOO_SMALL;
    ss.null_terminate();
  }

  return ::RtlInitUnicodeStringEx(command_line, command_buffer);
}

// --- publish_handles sub-functions ---

// Snapshot the current PEB handles and host cookie so they can be rolled back
// on failure or restored later.
void save_published_state(PublishedState *saved,
                                      RTL_USER_PROCESS_PARAMETERS *params,
                                      HANDLE *old_reference,
                                      ULONG_PTR *old_cookie,
                                      bool *has_old_cookie) {
  if (old_reference)
    *old_reference = load_current_console_reference();
  *old_cookie = 0;
  *has_old_cookie = false;
  if (saved) {
    saved->console_handle = params->ConsoleHandle;
    saved->console_reference = load_current_console_reference();
    saved->standard_input = params->StandardInput;
    saved->standard_output = params->StandardOutput;
    saved->standard_error = params->StandardError;
    NTSTATUS cookie_status = query_console_host_cookie(old_cookie);
    if (NT_SUCCESS(cookie_status)) {
      saved->console_host_cookie = *old_cookie;
      saved->has_console_host_cookie = true;
      *has_old_cookie = true;
    } else {
      saved->console_host_cookie = 0;
      saved->has_console_host_cookie = false;
    }
  } else {
    NTSTATUS cookie_status = query_console_host_cookie(old_cookie);
    *has_old_cookie = NT_SUCCESS(cookie_status);
  }
}

// Duplicate the ConDrv client handles (connection, reference, input, output,
// error) into the current process. On success all published_* out-params are
// populated; on failure the caller must close any non-null handles.
NTSTATUS duplicate_publish_handles(
    const condrv::CONDRV_CLIENT_HANDLES &handles, HANDLE reference,
    HANDLE *published_console, HANDLE *published_reference,
    HANDLE *published_input, HANDLE *published_output,
    HANDLE *published_error) {
  *published_console = nullptr;
  *published_reference = nullptr;
  *published_input = nullptr;
  *published_output = nullptr;
  *published_error = nullptr;

  NTSTATUS status =
      duplicate_process_handle(handles.Connection, published_console);
  if (!NT_SUCCESS(status))
    return status;

  if (reference) {
    status = duplicate_process_handle(reference, published_reference);
    if (!NT_SUCCESS(status))
      return status;
  }

  status = duplicate_process_handle(handles.Input, published_input);
  if (!NT_SUCCESS(status))
    return status;

  status = duplicate_process_handle(handles.Output, published_output);
  if (!NT_SUCCESS(status))
    return status;

  status = condrv::duplicate_condrv_output_as_error(*published_output,
                                                    published_error, false);
  if (!NT_SUCCESS(status)) {
    status = duplicate_process_handle(handles.Error, published_error);
    if (!NT_SUCCESS(status))
      return status;
  }

  return STATUS_SUCCESS;
}

// Write the published handles into PEB, update the fd table, and write the
// host cookie. On success, records the published handles in *saved
// (if non-null).
NTSTATUS commit_published_state(
    RTL_USER_PROCESS_PARAMETERS *params, HANDLE published_console,
    HANDLE published_reference, HANDLE published_input,
    HANDLE published_output, HANDLE published_error, ULONG64 server_pid,
    PublishedState *saved) {
  params->ConsoleHandle = published_console;

  if (published_input)
    params->StandardInput = published_input;
  if (published_output)
    params->StandardOutput = published_output;
  if (published_error)
    params->StandardError = published_error;

  internal::fd_table.rebind_std_fds_as_console(params->StandardInput,
                                               params->StandardOutput,
                                               params->StandardError);
  internal::console_tty::reset_buffered_input();
  internal::console_tty::reset_terminal_signal_state();
  internal::console_tty::adopt_controlling_terminal();

  ULONG_PTR cookie =
      static_cast<ULONG_PTR>(server_pid | CONSOLE_HOST_COOKIE_ACTIVE_BIT);

  NTSTATUS status = set_console_host_cookie(cookie);
  if (NT_SUCCESS(status)) {
    store_current_console_reference(published_reference);
    if (saved) {
      saved->published_reference = published_reference;
      saved->published_console_handle = published_console;
      saved->published_standard_input = published_input;
      saved->published_standard_output = published_output;
      saved->published_standard_error = published_error;
    }
  }
  return status;
}

// Roll back PEB handles, fd table, terminal state, and host cookie to the
// values captured before publish_handles began.
void rollback_published_state(RTL_USER_PROCESS_PARAMETERS *params,
                                          HANDLE old_console, HANDLE old_input,
                                          HANDLE old_output, HANDLE old_error,
                                          HANDLE old_reference,
                                          ULONG_PTR old_cookie,
                                          bool has_old_cookie) {
  params->ConsoleHandle = old_console;
  params->StandardInput = old_input;
  params->StandardOutput = old_output;
  params->StandardError = old_error;
  if (is_live_console_handle(old_console)) {
    internal::fd_table.rebind_std_fds_as_console(old_input, old_output,
                                                 old_error);
  } else {
    internal::fd_table.rebind_std_fds(old_input, old_output, old_error);
  }
  if (old_console)
    internal::console_tty::adopt_controlling_terminal();
  else
    internal::terminal_ops::detach_controlling_terminal();
  store_current_console_reference(old_reference);
  if (has_old_cookie)
    (void)set_console_host_cookie(old_cookie);
}

} // namespace

// ConDrv Ctrl event callback — invoked on a dedicated thread injected by the
// kernel (CtrlRoutine APC). After delivering the POSIX signal equivalent, the
// thread must self-terminate: returning from this function would unwind into
// ntdll's BaseThreadInitThunk which expects a normal thread — but this is a
// fire-and-forget APC fiber with no meaningful return path. NtTerminateThread
// cleanly exits the injected thread; __builtin_unreachable tells the compiler
// no code follows.
WINAPI DWORD ctrl_dispatch(ULONG_PTR raw_param) {
  DWORD ctrl_type = static_cast<DWORD>(raw_param) & CTRL_EVENT_TYPE_MASK;

  int signum = ctrl_event_to_signal(ctrl_type);
  if (signum != 0)
    signal_state::deliver_process_signal(signum);

  ::NtTerminateThread(NtCurrentThread(), 0);
  __builtin_unreachable();
}

NTSTATUS publish_handles(const condrv::CONDRV_CLIENT_HANDLES &handles,
                         HANDLE reference, PublishedState *saved) {
  auto *params = NtCurrentPeb()->ProcessParameters;
  if (!params || !handles.Connection)
    return STATUS_INVALID_PARAMETER;

  // 1. Snapshot current state for rollback.
  HANDLE old_console = params->ConsoleHandle;
  HANDLE old_input = params->StandardInput;
  HANDLE old_output = params->StandardOutput;
  HANDLE old_error = params->StandardError;
  HANDLE old_reference = nullptr;
  ULONG_PTR old_cookie = 0;
  bool has_old_cookie = false;
  save_published_state(saved, params, &old_reference, &old_cookie,
                       &has_old_cookie);

  // 2. Duplicate all ConDrv handles into the current process.
  HANDLE pub_console = nullptr, pub_reference = nullptr;
  HANDLE pub_input = nullptr, pub_output = nullptr, pub_error = nullptr;

  auto close_published = cpp::make_scope_guard([&] {
    close_handle_if_valid(&pub_error);
    close_handle_if_valid(&pub_output);
    close_handle_if_valid(&pub_input);
    close_handle_if_valid(&pub_reference);
    close_handle_if_valid(&pub_console);
  });

  NTSTATUS status = duplicate_publish_handles(
      handles, reference, &pub_console, &pub_reference, &pub_input,
      &pub_output, &pub_error);
  if (!NT_SUCCESS(status))
    return status;

  // 3. Query server PID (needed for cookie) before committing.
  ULONG64 server_pid = 0;
  status = condrv::get_condrv_server_pid(handles.Connection, &server_pid);
  if (!NT_SUCCESS(status))
    return status;

  // 4. Commit to PEB, fd table, and host cookie.
  status = commit_published_state(params, pub_console, pub_reference,
                                  pub_input, pub_output, pub_error,
                                  server_pid, saved);
  if (!NT_SUCCESS(status)) {
    rollback_published_state(params, old_console, old_input, old_output,
                             old_error, old_reference, old_cookie,
                             has_old_cookie);
    return status;
  }

  // Success — published handles are now owned by the PEB.
  close_published.dismiss();
  return STATUS_SUCCESS;
}

NTSTATUS restore_handles(PublishedState *saved) {
  auto *params = NtCurrentPeb()->ProcessParameters;
  if (!params || !saved)
    return STATUS_INVALID_PARAMETER;

  params->ConsoleHandle = saved->console_handle;
  params->StandardInput = saved->standard_input;
  params->StandardOutput = saved->standard_output;
  params->StandardError = saved->standard_error;
  if (is_live_console_handle(saved->console_handle)) {
    internal::fd_table.rebind_std_fds_as_console(
        params->StandardInput, params->StandardOutput, params->StandardError);
  } else {
    internal::fd_table.rebind_std_fds(params->StandardInput,
                                      params->StandardOutput,
                                      params->StandardError);
  }
  if (saved->console_handle)
    internal::console_tty::adopt_controlling_terminal();
  else
    internal::terminal_ops::detach_controlling_terminal();

  store_current_console_reference(saved->console_reference);

  close_handle_if_valid(&saved->published_standard_error);
  close_handle_if_valid(&saved->published_standard_output);
  close_handle_if_valid(&saved->published_standard_input);
  close_handle_if_valid(&saved->published_reference);
  close_handle_if_valid(&saved->published_console_handle);

  if (!saved->has_console_host_cookie)
    return STATUS_SUCCESS;
  return set_console_host_cookie(saved->console_host_cookie);
}

HANDLE current_reference() {
  ::LIBC_NAMESPACE::internal::g_console_init.ensure();
  return load_current_console_reference();
}

void set_current_reference(HANDLE reference) {
  store_current_console_reference(reference);
}

NTSTATUS sync_process_console_state() {
  // current_reference tracks only libc-owned explicit console references
  // (published or inherited PTY/console session attachments). The PEB
  // ConsoleHandle is a different handle class and must not be reinterpreted
  // as a console reference for child launch.
  return sync_process_console_host_cookie_from_peb();
}

NTSTATUS build_server_message(condrv::CONSOLE_SERVER_MSG *msg,
                              const SessionOptions &options) {
  if (!msg)
    return STATUS_INVALID_PARAMETER;

  if (options.initial_server_message) {
    *msg = *options.initial_server_message;
    return STATUS_SUCCESS;
  }

  auto *params = NtCurrentPeb()->ProcessParameters;
  if (!params)
    return STATUS_INVALID_PARAMETER;

  *msg = {};

  msg->StartupFlags = params->WindowFlags;
  msg->FillAttribute = static_cast<USHORT>(params->FillAttribute);
  msg->ShowWindow = static_cast<USHORT>(params->ShowWindowFlags);
  msg->ScreenBufferSize.X = static_cast<SHORT>(params->CountCharsX);
  msg->ScreenBufferSize.Y = static_cast<SHORT>(params->CountCharsY);
  msg->WindowSize.X = static_cast<SHORT>(params->CountX);
  msg->WindowSize.Y = static_cast<SHORT>(params->CountY);
  msg->WindowOrigin.X = static_cast<SHORT>(params->StartingX);
  msg->WindowOrigin.Y = static_cast<SHORT>(params->StartingY);

  ULONG process_group_id = params->ProcessGroupId;
  if (process_group_id == 0)
    process_group_id = current_process_id();
  msg->ProcessGroupId = process_group_id;
  msg->ConsoleApp = options.console_app;
  msg->WindowVisible = options.window_visible;

  NTSTATUS status =
      copy_field_with_fallback(msg->Title, &msg->TitleLength, options.title,
                               params->WindowTitle, DEFAULT_CONSOLE_TITLE);
  if (!NT_SUCCESS(status))
    return status;

  status = copy_field_with_fallback(msg->ApplicationName,
                                    &msg->ApplicationNameLength,
                                    options.application_name,
                                    params->ImagePathName);
  if (!NT_SUCCESS(status))
    return status;

  return copy_field_with_fallback(msg->CurrentDirectory,
                                  &msg->CurrentDirectoryLength,
                                  options.current_directory,
                                  params->CurrentDirectory.DosPath);
}

NTSTATUS publish(Session *session) {
  if (!session || !session->handles.Connection)
    return STATUS_INVALID_PARAMETER;

  if (session->published)
    return STATUS_SUCCESS;

  NTSTATUS status =
      publish_handles(session->handles, session->reference,
                      &session->saved_published_state);
  if (NT_SUCCESS(status))
    session->published = true;
  return status;
}

NTSTATUS restore(Session *session) {
  if (!session)
    return STATUS_INVALID_PARAMETER;

  if (!session->published)
    return STATUS_SUCCESS;

  NTSTATUS status = restore_handles(&session->saved_published_state);
  if (NT_SUCCESS(status))
    session->published = false;
  return status;
}

NTSTATUS close(Session *session, bool deliver_sighup) {
  if (!session)
    return STATUS_INVALID_PARAMETER;

  bool should_signal_hangup = deliver_sighup && session->published &&
                              session->saved_published_state.console_handle ==
                                  nullptr;

  // close_owned() handles restore + handle cleanup. Propagate the restore
  // error (if any) but always complete the close regardless.
  NTSTATUS result = session->close_owned();

  if (should_signal_hangup)
    internal::console_tty::deliver_terminal_hangup();
  return result;
}

NTSTATUS launch(Session *session, const SessionOptions &options) {
  if (!session)
    return STATUS_INVALID_PARAMETER;

  NTSTATUS status = close(session);
  if (!NT_SUCCESS(status))
    return status;

  condrv::CONSOLE_SERVER_MSG server_message = {};
  status = build_server_message(&server_message, options);
  if (!NT_SUCCESS(status))
    return status;

  // All resources are held in locals until fully committed to the session.
  // The scope guard only closes locals — the session is never partially filled.
  HANDLE server = nullptr;
  HANDLE reference = nullptr;
  HANDLE connection = nullptr;
  condrv::CONDRV_CLIENT_HANDLES handles = {};

  auto cleanup = cpp::make_scope_guard([&] {
    condrv::close_condrv_client_handles(&handles);
    close_handle_if_valid(&connection);
    close_handle_if_valid(&reference);
    close_handle_if_valid(&server);
  });

  // Stack budget: image_buffer (640 B) + command_buffer (~684 B) = ~1.3 KB.
  // These are the largest stack allocations in the console launch path. If
  // launch() is ever called from a fiber or thread with a reduced stack, this
  // budget may need to move to a heap or section-backed allocation.
  WCHAR image_buffer[DEFAULT_CONHOST_IMAGE_BUFFER_CAPACITY] = {};
  WCHAR command_buffer[DEFAULT_CONHOST_COMMAND_BUFFER_CAPACITY] = {};
  UNICODE_STRING image_path = {};
  UNICODE_STRING command_line = {};

  status = condrv::open_condrv_server(&server, GENERIC_ALL, false);
  if (!NT_SUCCESS(status))
    return status;

  status = condrv::open_condrv_child(&reference, server,
                                     condrv::CONDRV_REFERENCE_NAME);
  if (!NT_SUCCESS(status))
    return status;

  status = build_default_conhost_strings(options, &image_path, &command_line,
                                         image_buffer,
                                         DEFAULT_CONHOST_IMAGE_BUFFER_CAPACITY,
                                         command_buffer,
                                         DEFAULT_CONHOST_COMMAND_BUFFER_CAPACITY);
  if (!NT_SUCCESS(status))
    return status;

  status =
      condrv::launch_condrv_server_with_params(server, &image_path, &command_line);
  if (!NT_SUCCESS(status))
    return status;

  status = condrv::create_condrv_connect(&connection, server, server_message,
                                         options.inherit_client_handles);
  if (!NT_SUCCESS(status))
    return status;

  status = condrv::create_condrv_client_handles(
      &handles, connection, options.inherit_client_handles);
  if (!NT_SUCCESS(status))
    return status;
  connection = nullptr; // ownership absorbed into handles.Connection

  // All resources acquired. Transfer ownership to session atomically.
  session->server = server;
  session->reference = reference;
  session->handles = handles;

  // Null locals so the scope guard won't close them — session now owns them.
  server = nullptr;
  reference = nullptr;
  handles = {};

  cleanup.dismiss();

  if (options.publish_to_process) {
    status = publish(session);
    if (!NT_SUCCESS(status)) {
      // publish failed — session owns the handles, close_owned() cleans up.
      session->close_owned();
      return status;
    }
  }

  return STATUS_SUCCESS;
}

NTSTATUS attach(Session *session, HANDLE process_id,
                const SessionOptions &options) {
  if (!session || !process_id)
    return STATUS_INVALID_PARAMETER;

  NTSTATUS status = close(session);
  if (!NT_SUCCESS(status))
    return status;

  condrv::CONSOLE_SERVER_MSG server_message = {};
  status = build_server_message(&server_message, options);
  if (!NT_SUCCESS(status))
    return status;

  HANDLE connection = nullptr;
  condrv::CONDRV_CLIENT_HANDLES handles = {};

  auto cleanup = cpp::make_scope_guard([&] {
    condrv::close_condrv_client_handles(&handles);
    close_handle_if_valid(&connection);
  });

  status = condrv::create_condrv_attach(&connection, process_id, server_message,
                                        options.inherit_client_handles);
  if (!NT_SUCCESS(status))
    return status;

  status = condrv::create_condrv_client_handles(
      &handles, connection, options.inherit_client_handles);
  if (!NT_SUCCESS(status))
    return status;
  connection = nullptr; // ownership absorbed into handles.Connection

  // Transfer ownership to session, null locals.
  session->handles = handles;
  handles = {};

  cleanup.dismiss();

  if (options.publish_to_process) {
    status = publish(session);
    if (!NT_SUCCESS(status)) {
      session->close_owned();
      return status;
    }
  }

  return STATUS_SUCCESS;
}

NTSTATUS Session::close_owned() {
  if (!handles.Connection && !server && !reference)
    return STATUS_SUCCESS;

  NTSTATUS result = STATUS_SUCCESS;
  if (published) {
    result = restore_handles(&saved_published_state);
    published = false;
  }

  condrv::close_condrv_client_handles(&handles);
  if (reference) {
    ::NtClose(reference);
    reference = nullptr;
  }
  if (server) {
    ::NtClose(server);
    server = nullptr;
  }
  return result;
}

} // namespace console
} // namespace LIBC_NAMESPACE_DECL

LIBC_REGISTER_LAZY_RESET(console,
                         ::LIBC_NAMESPACE::internal::g_console_init)
