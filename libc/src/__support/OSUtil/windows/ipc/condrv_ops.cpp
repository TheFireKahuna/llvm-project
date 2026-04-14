//===-- ConDrv complex out-of-line operations --------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Multi-step ConDrv operations that are too large for inline expansion.
// These functions have normal linkage (no LIBC_INLINE).
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/ipc/condrv_ops.h"
#include "src/__support/OSUtil/windows/nt/nt_process.h"

namespace LIBC_NAMESPACE_DECL {
namespace condrv {

//===----------------------------------------------------------------------===//
// Shared rollback helper for client-handle creation
//===----------------------------------------------------------------------===//

// Completes the client-handle bundle after Input and Output have been opened.
// Duplicates Output as Error and, on any failure, rolls back all handles.
static NTSTATUS finish_client_handles(CONDRV_CLIENT_HANDLES *handles,
                                      HANDLE input, HANDLE output,
                                      HANDLE connection_root, bool inherit) {
  handles->Input = input;
  handles->Output = output;

  NTSTATUS status =
      duplicate_condrv_output_as_error(output, &handles->Error, inherit);
  if (!NT_SUCCESS(status)) {
    ::NtClose(output);
    ::NtClose(input);
    handles->Output = nullptr;
    handles->Input = nullptr;
    return status;
  }

  handles->Connection = connection_root;
  return STATUS_SUCCESS;
}

//===----------------------------------------------------------------------===//
// WriteConsole
//===----------------------------------------------------------------------===//

// Low-level typed WriteConsole transport. KernelBase's current wrappers target
// the passed CONOUT$ / screen-buffer handle directly and use Client = NULL.
NTSTATUS write_console_bytes(HANDLE object_handle, const void *buffer,
                             DWORD bytes_to_write, bool unicode,
                             DWORD *bytes_written) {
  if (bytes_written)
    *bytes_written = 0;
  if (!object_handle)
    return STATUS_INVALID_HANDLE;
  if (bytes_to_write != 0 && !buffer)
    return STATUS_INVALID_PARAMETER;

  struct Packet {
    CONSOLE_MSG_HEADER header;
    CONSOLE_WRITECONSOLE_MSG payload;
  } packet = {{OP_WRITE_CONSOLE,
               static_cast<ULONG>(sizeof(CONSOLE_WRITECONSOLE_MSG))},
              {0, unicode ? BOOLEAN(1) : BOOLEAN(0), {0, 0, 0}}};

  CD_WRITECONSOLE_USER_IO req = {nullptr,
                                 2,
                                 1,
                                 {{sizeof(packet), &packet.header},
                                  {bytes_to_write, const_cast<void *>(buffer)},
                                  {sizeof(packet.payload), &packet.payload}}};

  NTSTATUS status = issue_user_io_request_on(object_handle, req);
  if (status == STATUS_INVALID_PARAMETER)
    status = STATUS_INVALID_HANDLE;
  if (NT_SUCCESS(status) && bytes_written)
    *bytes_written = packet.payload.NumBytes;
  return status;
}

//===----------------------------------------------------------------------===//
// WriteConsoleInput
//===----------------------------------------------------------------------===//

// Low-level typed WriteConsoleInput transport. The canonical KernelBase path
// targets the PEB ConsoleHandle and routes the passed CONIN$ handle through the
// Client field. The public wrappers hardcode Append = 1; this helper exposes
// it directly because the transport field is real.
NTSTATUS write_console_input_on(
    HANDLE io_target, const CONSOLE_WRITECONSOLEINPUT_REQUEST &request,
    CONSOLE_WRITECONSOLEINPUT_RESULT *result) {
  if (result)
    *result = {};
  if (!request.ObjectHandle)
    return STATUS_INVALID_HANDLE;
  if (request.RecordCount != 0 && !request.Records)
    return STATUS_INVALID_PARAMETER;

  HANDLE target = io_target ? io_target : get_console_handle();
  if (!target)
    return STATUS_INVALID_HANDLE;

  constexpr ULONG MAX_CONSOLE_INPUT_RECORDS =
      0xFFFFFFFFu / static_cast<ULONG>(sizeof(CONSOLE_INPUT_RECORD));
  if (request.RecordCount > MAX_CONSOLE_INPUT_RECORDS)
    return STATUS_INVALID_PARAMETER;

  ULONG record_bytes =
      request.RecordCount * static_cast<ULONG>(sizeof(CONSOLE_INPUT_RECORD));
  struct Packet {
    CONSOLE_MSG_HEADER header;
    CONSOLE_WRITECONSOLEINPUT_MSG payload;
  } packet = {{OP_WRITE_CONSOLE_INPUT,
               static_cast<ULONG>(sizeof(CONSOLE_WRITECONSOLEINPUT_MSG))},
              {0, request.Unicode, request.Append, {0, 0}}};

  CD_WRITECONSOLEINPUT_USER_IO req = {
      request.ObjectHandle,
      2,
      1,
      {
          {sizeof(packet), &packet.header},
          {record_bytes, const_cast<CONSOLE_INPUT_RECORD *>(request.Records)},
          {sizeof(packet.payload), &packet.payload},
      },
  };

  NTSTATUS status = issue_user_io_request_on(target, req);
  if (NT_SUCCESS(status) && result)
    result->NumRecords = packet.payload.NumRecords;
  return status;
}

//===----------------------------------------------------------------------===//
// ReadConsole (cooked)
//===----------------------------------------------------------------------===//

// Low-level cooked ReadConsole transport. This is the fully decoded
// CD_USER_DEFINED_IO packet, not the public KernelBase wrapper policy:
//   - io_target defaults to request.ObjectHandle (canonical direct CONIN$ path)
//   - when io_target differs, the helper routes through request.ObjectHandle as
//     the Client handle to match the verified PEB ConsoleHandle path
//   - ExeNameLength is a WCHAR count, not a byte count
//   - SeedBufferSize and OutputBufferSize are raw byte counts
//   - SeedBuffer and OutputBuffer may alias, matching the public wrapper
//
// Callers own the policy choices here: exe-name sourcing, initial-seed use,
// CtrlWakeupMask, and ProcessControlZ. The public ReadConsoleA wrapper ignores
// the control block entirely; a direct ANSI packet with SeedBufferSize != 0 was
// not observed to succeed in the 24H2 decode probes, so callers should treat
// that combination as characterization-only until they intentionally verify it.
NTSTATUS read_console_cooked_on(HANDLE io_target,
                                const CONSOLE_READCONSOLE_REQUEST &request,
                                CONSOLE_READCONSOLE_RESULT *result) {
  if (result)
    *result = {};
  if (!request.ObjectHandle)
    return STATUS_INVALID_HANDLE;
  if (request.ExeNameLength != 0 && !request.ExeName)
    return STATUS_INVALID_PARAMETER;
  if (request.SeedBufferSize != 0 && !request.SeedBuffer)
    return STATUS_INVALID_PARAMETER;
  if (request.OutputBufferSize != 0 && !request.OutputBuffer)
    return STATUS_INVALID_PARAMETER;

  HANDLE target = io_target ? io_target : request.ObjectHandle;
  HANDLE client =
      target == request.ObjectHandle ? nullptr : request.ObjectHandle;
  CONSOLE_READCONSOLE_MSG payload = {
      request.Unicode,
      request.ProcessControlZ,
      request.ExeNameLength,
      request.SeedBufferSize,
      request.CtrlWakeupMask,
      0,
      request.OutputBufferSize,
  };

  struct Packet {
    CONSOLE_MSG_HEADER header;
    CONSOLE_READCONSOLE_MSG payload;
  } packet = {{OP_READ_CONSOLE,
               static_cast<ULONG>(sizeof(CONSOLE_READCONSOLE_MSG))},
              payload};

  CD_READCONSOLE_USER_IO req = {
      client,
      3,
      2,
      {
          {sizeof(packet), &packet.header},
          {static_cast<ULONG>(request.ExeNameLength * sizeof(WCHAR)),
           const_cast<WCHAR *>(request.ExeName)},
          {request.SeedBufferSize, const_cast<void *>(request.SeedBuffer)},
          {sizeof(packet.payload), &packet.payload},
          {request.OutputBufferSize, request.OutputBuffer},
      },
  };

  NTSTATUS status = issue_user_io_request_on(target, req);
  if (NT_SUCCESS(status) && result) {
    result->NumBytes = packet.payload.NumBytes;
    result->ControlKeyState = packet.payload.ControlKeyState;
  }
  return status;
}

//===----------------------------------------------------------------------===//
// ReadConsoleInput
//===----------------------------------------------------------------------===//

// Low-level fixed-packet GetConsoleInput transport. This mirrors the live
// KernelBase packet layout exactly:
//   - object_handle = stdin-side console handle
//   - request_message_ptr -> { CONSOLE_MSG_HEADER, CONSOLE_GETCONSOLEINPUT_MSG }
//   - result_header_ptr aliases the params subrange of that same block
//   - record_buffer_ptr carries standard 20-byte INPUT_RECORD entries
//
// `flags` accepts CONSOLE_READ_NOREMOVE / CONSOLE_READ_NOWAIT. On Win11 24H2,
// a blocking CONSOLE_READ_NOREMOVE wait on an empty queue wakes by consuming
// the arriving event, matching ReadConsoleInputExW. Use peek_console_input()
// for stable non-consuming peek semantics.
NTSTATUS read_console_input_ex(HANDLE object_handle,
                               CONSOLE_INPUT_RECORD *records,
                               DWORD max_records, DWORD *record_count,
                               USHORT flags, bool unicode) {
  if (record_count)
    *record_count = 0;
  if (!object_handle)
    return STATUS_INVALID_HANDLE;
  if (!records || !record_count || max_records == 0)
    return STATUS_INVALID_PARAMETER;
  if ((flags & ~CONSOLE_READ_VALID) != 0)
    return STATUS_INVALID_PARAMETER;

  constexpr DWORD MAX_CONSOLE_INPUT_RECORDS =
      0xFFFFFFFFu / static_cast<DWORD>(sizeof(CONSOLE_INPUT_RECORD));
  if (max_records > MAX_CONSOLE_INPUT_RECORDS)
    return STATUS_INVALID_PARAMETER;

  HANDLE console = get_console_handle();
  if (!console)
    return STATUS_INVALID_HANDLE;

  struct Packet {
    CONSOLE_MSG_HEADER header;
    CONSOLE_GETCONSOLEINPUT_MSG params;
  } packet = {{OP_GET_CONSOLE_INPUT,
               static_cast<ULONG>(sizeof(CONSOLE_GETCONSOLEINPUT_MSG))},
              {0, flags, unicode ? BOOLEAN(1) : BOOLEAN(0), 0}};

  CD_GETCONSOLEINPUT_MESSAGE msg = {object_handle,
                                    1,
                                    2,
                                    sizeof(packet),
                                    0,
                                    &packet.header,
                                    sizeof(packet.params),
                                    0,
                                    &packet.params,
                                    static_cast<ULONG>(max_records * sizeof(CONSOLE_INPUT_RECORD)),
                                    0,
                                    records};

  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtDeviceIoControlFile(console, nullptr, nullptr, nullptr,
                                            &iosb, IOCTL_CONDRV_ISSUE_USER_IO,
                                            &msg, sizeof(msg), nullptr, 0);
  if (!NT_SUCCESS(status))
    return status;
  if (NT_SUCCESS(iosb.Status))
    *record_count = packet.params.NumRecords;
  return iosb.Status;
}

//===----------------------------------------------------------------------===//
// GenerateConsoleCtrlEvent
//===----------------------------------------------------------------------===//

NTSTATUS generate_console_ctrl_event(DWORD event_type,
                                     DWORD process_group_id) {
  HANDLE console = get_console_handle();
  if (!console)
    return STATUS_INVALID_HANDLE;

  struct Packet {
    CONSOLE_MSG_HEADER header;
    CONSOLE_GENERATE_CTRL_EVENT_MSG payload;
  } packet = {{API_NUMBER_GENERATE_CTRL_EVENT,
               sizeof(CONSOLE_GENERATE_CTRL_EVENT_MSG)},
              {event_type, process_group_id}};

  unsigned char *payload_bytes =
      reinterpret_cast<unsigned char *>(&packet.payload);
  CD_CLIENT_MESSAGE msg = {nullptr,
                           1,
                           1,
                           sizeof(packet),
                           0,
                           &packet.header,
                           sizeof(packet.payload),
                           0,
                           payload_bytes};
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtDeviceIoControlFile(console, nullptr, nullptr, nullptr,
                                            &iosb, IOCTL_CONDRV_ISSUE_USER_IO,
                                            &msg, sizeof(msg), nullptr, 0);
  if (!NT_SUCCESS(status))
    return status;
  return iosb.Status;
}

//===----------------------------------------------------------------------===//
// Connection and client-handle creation
//===----------------------------------------------------------------------===//

// Create a \Connect handle with the decoded "server" EA payload.
NTSTATUS create_condrv_connect(HANDLE *out, HANDLE root,
                               const CONSOLE_SERVER_MSG &server_msg,
                               bool inherit) {
  auto ea_s = internal::byte_scratch(CONDRV_SERVER_EA_SIZE);
  if (!ea_s)
    return STATUS_NO_MEMORY;
  auto *ea = reinterpret_cast<unsigned char *>(ea_s.data());
  build_condrv_server_ea(ea, server_msg);

  if (root)
    return create_condrv_child(out, root, CONDRV_CONNECT_NAME,
                               FILE_GENERIC_READ | FILE_GENERIC_WRITE |
                                   SYNCHRONIZE,
                               FILE_SYNCHRONOUS_IO_NONALERT, inherit, ea,
                               CONDRV_SERVER_EA_SIZE);

  return create_condrv_path_on(out, nullptr, CONDRV_CONNECT_PATH,
                               FILE_GENERIC_READ | FILE_GENERIC_WRITE |
                                   SYNCHRONIZE,
                               FILE_SYNCHRONOUS_IO_NONALERT, inherit,
                               FILE_CREATE, ea, CONDRV_SERVER_EA_SIZE);
}

// Builds the standard client-handle bundle and adopts connection_root on
// success.
NTSTATUS create_condrv_client_handles(CONDRV_CLIENT_HANDLES *handles,
                                      HANDLE connection_root, bool inherit) {
  if (!handles || !connection_root)
    return STATUS_INVALID_PARAMETER;

  HANDLE input = nullptr;
  NTSTATUS status = create_condrv_input(&input, connection_root, inherit);
  if (!NT_SUCCESS(status))
    return status;

  HANDLE output = nullptr;
  status = create_condrv_output(&output, connection_root, inherit);
  if (!NT_SUCCESS(status)) {
    ::NtClose(input);
    return status;
  }

  return finish_client_handles(handles, input, output, connection_root,
                               inherit);
}

NTSTATUS open_condrv_client_handles(CONDRV_CLIENT_HANDLES *handles,
                                    HANDLE connection_root, bool inherit) {
  if (!handles || !connection_root)
    return STATUS_INVALID_PARAMETER;

  HANDLE input = nullptr;
  NTSTATUS status = open_condrv_input(&input, connection_root, inherit);
  if (!NT_SUCCESS(status))
    return status;

  HANDLE output = nullptr;
  status = open_condrv_output(&output, connection_root, inherit);
  if (!NT_SUCCESS(status)) {
    ::NtClose(input);
    return status;
  }

  return finish_client_handles(handles, input, output, connection_root,
                               inherit);
}

NTSTATUS launch_condrv_server_with_params(
    HANDLE server_handle, const UNICODE_STRING *image_path,
    const UNICODE_STRING *command_line, PVOID environment,
    const UNICODE_STRING *desktop_info) {
  if (!server_handle || !image_path || !command_line)
    return STATUS_INVALID_PARAMETER;

  RTL_USER_PROCESS_PARAMETERS *params = nullptr;
  auto *current = NtCurrentPeb()->ProcessParameters;
  const UNICODE_STRING *desktop =
      desktop_info ? desktop_info : (current ? &current->DesktopInfo : nullptr);
  NTSTATUS status = ::RtlCreateProcessParametersEx(
      &params, image_path, nullptr, nullptr, command_line, environment, nullptr,
      desktop, nullptr, nullptr, RTL_USER_PROC_PARAMS_NORMALIZED);
  if (!NT_SUCCESS(status))
    return status;

  status = launch_condrv_server(server_handle, params);
  ::RtlDestroyProcessParameters(params);
  return status;
}

} // namespace condrv
} // namespace LIBC_NAMESPACE_DECL
