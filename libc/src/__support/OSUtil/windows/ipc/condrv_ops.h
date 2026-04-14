//===-- ConDrv inline operations and dispatch helpers ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Small inline accessors, template dispatch helpers, and thin wrappers for
// the ConDrv console driver interface. Complex multi-step operations are
// declared here but defined in condrv_ops.cpp.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_CONDRV_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_CONDRV_OPS_H

#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/OSUtil/windows/ipc/condrv_types.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/nt_file.h"
#include "src/__support/OSUtil/windows/nt/nt_process.h"
#include "src/__support/macros/attributes.h"

namespace LIBC_NAMESPACE_DECL {
namespace condrv {

//===----------------------------------------------------------------------===//
// Inline helpers
//===----------------------------------------------------------------------===//

// Helper: initialize a UNICODE_STRING from a static WCHAR array.
template <unsigned N>
LIBC_INLINE void init_unicode(UNICODE_STRING *us, const WCHAR (&str)[N]) {
  us->Length = static_cast<USHORT>((N - 1) * sizeof(WCHAR));
  us->MaximumLength = static_cast<USHORT>(N * sizeof(WCHAR));
  us->Buffer = const_cast<WCHAR *>(str);
}

// Get the ConDrv server handle from PEB.
LIBC_INLINE HANDLE get_console_handle() {
  auto *params = NtCurrentPeb()->ProcessParameters;
  return params ? params->ConsoleHandle : nullptr;
}

template <unsigned NumBuffers>
LIBC_INLINE constexpr ULONG user_defined_io_size() {
  return static_cast<ULONG>(sizeof(HANDLE) + 2 * sizeof(ULONG) +
                            NumBuffers * sizeof(CD_IO_BUFFER));
}

// Fixed-size C++ wrapper around Terminal's CD_USER_DEFINED_IO request shape.
// The verified ReadConsole / WriteConsole paths use this transport and target
// the passed CONIN$ / CONOUT$ handle directly. Canonical packets may use
// Client = NULL, so this helper does not reject it.
template <unsigned NumBuffers>
LIBC_INLINE NTSTATUS
issue_user_io_request_on(HANDLE io_target,
                         const CD_USER_DEFINED_IO_STACK<NumBuffers> &req) {
  if (!io_target)
    return STATUS_INVALID_HANDLE;

  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtDeviceIoControlFile(
      io_target, nullptr, nullptr, nullptr, &iosb, IOCTL_CONDRV_ISSUE_USER_IO,
      const_cast<CD_USER_DEFINED_IO_STACK<NumBuffers> *>(&req),
      user_defined_io_size<NumBuffers>(), nullptr, 0);
  if (!NT_SUCCESS(status))
    return status;
  return iosb.Status;
}

template <unsigned NumBuffers>
LIBC_INLINE NTSTATUS
issue_user_io_request(const CD_USER_DEFINED_IO_STACK<NumBuffers> &req,
                      HANDLE io_target = nullptr) {
  HANDLE target = io_target ? io_target : get_console_handle();
  return issue_user_io_request_on(target, req);
}

// Direct handle-local IOCTL query helper. Display/font IOCTL success on client
// handles is still characterization-only and not yet a proven stable ABI.
LIBC_INLINE NTSTATUS query_console_ioctl(HANDLE object_handle, ULONG ioctl,
                                         void *out_buffer, ULONG out_size) {
  if (!object_handle || !out_buffer)
    return STATUS_INVALID_HANDLE;

  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status =
      ::NtDeviceIoControlFile(object_handle, nullptr, nullptr, nullptr, &iosb,
                              ioctl, nullptr, 0, out_buffer, out_size);
  if (!NT_SUCCESS(status))
    return status;
  return iosb.Status;
}

// Fixed-packet helper for the native 0x30-byte CD_CLIENT_MESSAGE transport.
// Win11 24H2 probes show two routing modes:
//   - io_target = PEB ConsoleHandle, object_handle = CONIN$/CONOUT$
//   - io_target = direct child handle, object_handle ignored (may be NULL)
template <typename Payload>
LIBC_INLINE NTSTATUS issue_console_message_on(HANDLE io_target,
                                              HANDLE object_handle,
                                              ULONG api_number,
                                              Payload &payload) {
  HANDLE target = io_target ? io_target : get_console_handle();
  if (!target)
    return STATUS_INVALID_HANDLE;

  constexpr ULONG packet_size =
      static_cast<ULONG>(sizeof(CONSOLE_MSG_HEADER) + sizeof(Payload));
  struct Packet {
    CONSOLE_MSG_HEADER header;
    Payload payload;
  } packet = {{api_number, static_cast<ULONG>(sizeof(Payload))}, payload};

  CD_CLIENT_MESSAGE msg = {object_handle,
                           1,
                           1,
                           packet_size,
                           0,
                           &packet.header,
                           sizeof(Payload),
                           0,
                           &packet.payload};
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtDeviceIoControlFile(target, nullptr, nullptr, nullptr,
                                            &iosb, IOCTL_CONDRV_ISSUE_USER_IO,
                                            &msg, sizeof(msg), nullptr, 0);
  if (!NT_SUCCESS(status))
    return status;
  if (NT_SUCCESS(iosb.Status))
    payload = packet.payload;
  return iosb.Status;
}

template <typename Payload>
LIBC_INLINE NTSTATUS issue_console_message(HANDLE object_handle,
                                           ULONG api_number,
                                           Payload &payload) {
  return issue_console_message_on(nullptr, object_handle, api_number, payload);
}

LIBC_INLINE NTSTATUS issue_console_message_on(HANDLE io_target,
                                              HANDLE object_handle,
                                              ULONG api_number) {
  HANDLE target = io_target ? io_target : get_console_handle();
  if (!target)
    return STATUS_INVALID_HANDLE;

  CONSOLE_MSG_HEADER header = {api_number, 0};
  unsigned char *header_end =
      reinterpret_cast<unsigned char *>(&header) + sizeof(header);
  CD_CLIENT_MESSAGE msg = {object_handle, 1, 1, sizeof(header), 0, &header,
                           0,           0, header_end};
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtDeviceIoControlFile(target, nullptr, nullptr, nullptr,
                                            &iosb, IOCTL_CONDRV_ISSUE_USER_IO,
                                            &msg, sizeof(msg), nullptr, 0);
  if (!NT_SUCCESS(status))
    return status;
  return iosb.Status;
}

LIBC_INLINE NTSTATUS issue_console_message(HANDLE object_handle,
                                           ULONG api_number) {
  return issue_console_message_on(nullptr, object_handle, api_number);
}

LIBC_INLINE void screen_buffer_info_payload_to_ex(
    const CONSOLE_SCREENBUFFERINFO_MSG &payload,
    CONSOLE_SCREEN_BUFFER_INFO_EX *info) {
  if (!info)
    return;

  info->cbSize = sizeof(*info);
  info->dwSize = payload.Size;
  info->dwCursorPosition = payload.CursorPosition;
  info->wAttributes = payload.Attributes;
  info->srWindow.Left = payload.ScrollPosition.X;
  info->srWindow.Top = payload.ScrollPosition.Y;
  info->srWindow.Right = payload.ScrollPosition.X + payload.CurrentWindowSize.X -
                         1;
  info->srWindow.Bottom =
      payload.ScrollPosition.Y + payload.CurrentWindowSize.Y - 1;
  info->dwMaximumWindowSize = payload.MaximumWindowSize;
  info->wPopupAttributes = payload.PopupAttributes;
  info->bFullscreenSupported = payload.FullscreenSupported;
  for (unsigned index = 0; index < 16; ++index)
    info->ColorTable[index] = payload.ColorTable[index];
}

// Mirrors the modern 24H2 KernelBase SetConsoleScreenBufferInfoEx wrapper
// exactly. This is intentionally not named as a generic inverse of the getter:
// live probes show the wrapper zeroes ScrollPosition/FullscreenSupported,
// copies Size/MaximumWindowSize even though the server ignores them in the
// tested matrix, and writes CurrentWindowSize.Y as Bottom-Top even though the
// server treats that field as the final visible height.
LIBC_INLINE void screen_buffer_info_ex_to_set_payload_wrapper_semantics(
    const CONSOLE_SCREEN_BUFFER_INFO_EX &info,
    CONSOLE_SCREENBUFFERINFO_MSG *payload) {
  if (!payload)
    return;

  *payload = {};
  payload->Size = info.dwSize;
  payload->CursorPosition = info.dwCursorPosition;
  payload->Attributes = info.wAttributes;
  payload->CurrentWindowSize.X = info.srWindow.Right - info.srWindow.Left;
  payload->CurrentWindowSize.Y = info.srWindow.Bottom - info.srWindow.Top;
  payload->MaximumWindowSize = info.dwMaximumWindowSize;
  payload->PopupAttributes = info.wPopupAttributes;
  for (unsigned index = 0; index < 16; ++index)
    payload->ColorTable[index] = info.ColorTable[index];
}

//===----------------------------------------------------------------------===//
// get_*/set_* wrapper functions
//===----------------------------------------------------------------------===//

LIBC_INLINE NTSTATUS get_console_screen_buffer_info_msg_on(
    HANDLE io_target, HANDLE object_handle, CONSOLE_SCREENBUFFERINFO_MSG *info) {
  if (!info)
    return STATUS_INVALID_PARAMETER;

  *info = {};
  return issue_console_message_on(io_target, object_handle,
                                  OP_GET_SCREEN_BUFFER_INFO, *info);
}

LIBC_INLINE NTSTATUS get_console_screen_buffer_info_msg(
    HANDLE object_handle, CONSOLE_SCREENBUFFERINFO_MSG *info) {
  return get_console_screen_buffer_info_msg_on(nullptr, object_handle, info);
}

LIBC_INLINE NTSTATUS set_console_screen_buffer_info_msg_on(
    HANDLE io_target, HANDLE object_handle,
    const CONSOLE_SCREENBUFFERINFO_MSG *info) {
  if (!info)
    return STATUS_INVALID_PARAMETER;

  CONSOLE_SCREENBUFFERINFO_MSG payload = *info;
  return issue_console_message_on(io_target, object_handle,
                                  OP_SET_SCREEN_BUFFER_INFO, payload);
}

LIBC_INLINE NTSTATUS set_console_screen_buffer_info_msg(
    HANDLE object_handle, const CONSOLE_SCREENBUFFERINFO_MSG *info) {
  return set_console_screen_buffer_info_msg_on(nullptr, object_handle, info);
}

LIBC_INLINE NTSTATUS get_console_screen_buffer_info_ex_on(
    HANDLE io_target, HANDLE object_handle, CONSOLE_SCREEN_BUFFER_INFO_EX *info) {
  if (!info)
    return STATUS_INVALID_PARAMETER;

  CONSOLE_SCREENBUFFERINFO_MSG payload = {};
  NTSTATUS status =
      get_console_screen_buffer_info_msg_on(io_target, object_handle, &payload);
  if (NT_SUCCESS(status))
    screen_buffer_info_payload_to_ex(payload, info);
  return status;
}

LIBC_INLINE NTSTATUS get_console_screen_buffer_info_ex(
    HANDLE object_handle, CONSOLE_SCREEN_BUFFER_INFO_EX *info) {
  return get_console_screen_buffer_info_ex_on(nullptr, object_handle, info);
}

LIBC_INLINE NTSTATUS set_console_screen_buffer_info_ex_wrapper_on(
    HANDLE io_target, HANDLE object_handle,
    const CONSOLE_SCREEN_BUFFER_INFO_EX *info) {
  if (!info)
    return STATUS_INVALID_PARAMETER;

  CONSOLE_SCREENBUFFERINFO_MSG payload = {};
  screen_buffer_info_ex_to_set_payload_wrapper_semantics(*info, &payload);
  return set_console_screen_buffer_info_msg_on(io_target, object_handle,
                                               &payload);
}

LIBC_INLINE NTSTATUS set_console_screen_buffer_info_ex_wrapper(
    HANDLE object_handle, const CONSOLE_SCREEN_BUFFER_INFO_EX *info) {
  return set_console_screen_buffer_info_ex_wrapper_on(nullptr, object_handle,
                                                      info);
}

LIBC_INLINE NTSTATUS get_display_size(HANDLE object_handle,
                                      CD_IO_DISPLAY_SIZE *size) {
  return query_console_ioctl(object_handle, IOCTL_CONDRV_GET_DISPLAY_SIZE, size,
                             sizeof(*size));
}

LIBC_INLINE NTSTATUS get_font_size(HANDLE object_handle,
                                   CD_IO_FONT_SIZE *size) {
  return query_console_ioctl(object_handle, IOCTL_CONDRV_GET_FONT_SIZE, size,
                             sizeof(*size));
}

LIBC_INLINE NTSTATUS issue_console_mode_user_io_on(HANDLE io_target,
                                                   HANDLE object_handle,
                                                   ULONG op_code,
                                                   DWORD *value) {
  if (!value)
    return STATUS_INVALID_HANDLE;

  CONSOLE_MODE_MSG payload = {*value};
  NTSTATUS status =
      issue_console_message_on(io_target, object_handle, op_code, payload);
  if (NT_SUCCESS(status))
    *value = payload.Mode;
  return status;
}

LIBC_INLINE NTSTATUS issue_console_mode_user_io(HANDLE object_handle,
                                                ULONG op_code, DWORD *value) {
  return issue_console_mode_user_io_on(nullptr, object_handle, op_code, value);
}

LIBC_INLINE NTSTATUS get_console_mode_on(HANDLE io_target, HANDLE object_handle,
                                         DWORD *mode) {
  return issue_console_mode_user_io_on(io_target, object_handle,
                                       OP_GET_CONSOLE_MODE, mode);
}

LIBC_INLINE NTSTATUS get_console_mode(HANDLE object_handle, DWORD *mode) {
  return issue_console_mode_user_io(object_handle, OP_GET_CONSOLE_MODE, mode);
}

LIBC_INLINE NTSTATUS set_console_mode_on(HANDLE io_target, HANDLE object_handle,
                                         DWORD mode) {
  return issue_console_mode_user_io_on(io_target, object_handle,
                                       OP_SET_CONSOLE_MODE, &mode);
}

LIBC_INLINE NTSTATUS set_console_mode(HANDLE object_handle, DWORD mode) {
  return issue_console_mode_user_io(object_handle, OP_SET_CONSOLE_MODE, &mode);
}

LIBC_INLINE NTSTATUS get_console_code_page_on(HANDLE io_target,
                                              bool output,
                                              DWORD *code_page) {
  if (!code_page)
    return STATUS_INVALID_PARAMETER;

  // Win11 24H2 probes show codepage ops are console-global on this path:
  // target may be PEB ConsoleHandle, CONIN$, or CONOUT$, and object_handle is
  // ignored. ApiDescriptorSize must be 8.
  CONSOLE_GETCP_MSG payload = {0, output ? BOOLEAN(1) : BOOLEAN(0), {0, 0, 0}};
  NTSTATUS status = issue_console_message_on(io_target, nullptr,
                                             OP_GET_CONSOLE_CP, payload);
  if (NT_SUCCESS(status))
    *code_page = payload.CodePage;
  return status;
}

LIBC_INLINE NTSTATUS get_console_code_page(bool output, DWORD *code_page) {
  return get_console_code_page_on(nullptr, output, code_page);
}

LIBC_INLINE NTSTATUS set_console_code_page_on(HANDLE io_target, DWORD code_page,
                                              bool output) {
  // Split-buffer probes show the setter reads CodePage/Output from the
  // message payload and only echoes back through data_ptr.
  CONSOLE_SETCP_MSG payload = {code_page, output ? BOOLEAN(1) : BOOLEAN(0),
                               {0, 0, 0}};
  return issue_console_message_on(io_target, nullptr, OP_SET_CONSOLE_CP,
                                  payload);
}

LIBC_INLINE NTSTATUS set_console_code_page(DWORD code_page, bool output) {
  return set_console_code_page_on(nullptr, code_page, output);
}

// KernelBase probes this after SetConsoleOutputCP and calls SetThreadLocale on
// success. The transport is decoded, but live 24H2 probes saw some consoles
// return a console-specific STATUS_NOT_SUPPORTED here while SetConsoleOutputCP
// itself still succeeded.
LIBC_INLINE NTSTATUS get_console_lang_id_on(HANDLE io_target,
                                            LANGID *lang_id) {
  if (!lang_id)
    return STATUS_INVALID_PARAMETER;

  CONSOLE_LANGID_MSG payload = {};
  NTSTATUS status = issue_console_message_on(io_target, nullptr,
                                             OP_GET_CONSOLE_LANG_ID, payload);
  if (NT_SUCCESS(status))
    *lang_id = payload.LangId;
  return status;
}

LIBC_INLINE NTSTATUS get_console_lang_id(LANGID *lang_id) {
  return get_console_lang_id_on(nullptr, lang_id);
}

LIBC_INLINE NTSTATUS get_console_input_cp(DWORD *code_page) {
  return get_console_code_page(false, code_page);
}

LIBC_INLINE NTSTATUS get_console_output_cp(DWORD *code_page) {
  return get_console_code_page(true, code_page);
}

LIBC_INLINE NTSTATUS set_console_input_cp(DWORD code_page) {
  return set_console_code_page(code_page, false);
}

LIBC_INLINE NTSTATUS set_console_output_cp(DWORD code_page) {
  return set_console_code_page(code_page, true);
}

LIBC_INLINE NTSTATUS get_number_of_input_events(HANDLE object_handle,
                                                DWORD *ready_events) {
  if (!ready_events)
    return STATUS_INVALID_HANDLE;

  CONSOLE_GETNUMBEROFINPUTEVENTS_MSG payload = {};
  NTSTATUS status =
      issue_console_message(object_handle, OP_GET_NUMBER_OF_INPUT_EVENTS,
                            payload);
  if (NT_SUCCESS(status))
    *ready_events = payload.ReadyEvents;
  return status;
}

//===----------------------------------------------------------------------===//
// Template functions (path/child creation)
//===----------------------------------------------------------------------===//

template <unsigned N>
LIBC_INLINE NTSTATUS create_condrv_path_on(
    HANDLE *out, HANDLE root, const WCHAR (&path)[N], ACCESS_MASK desired_access,
    ULONG create_options, bool inherit, ULONG disposition,
    const void *ea_buffer = nullptr, ULONG ea_length = 0) {
  UNICODE_STRING name;
  init_unicode(&name, path);

  auto oa = windows::named_oa(&name, root, inherit);

  IO_STATUS_BLOCK iosb = {};
  return ::NtCreateFile(out, desired_access, &oa, &iosb, nullptr, 0,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        disposition, create_options,
                        const_cast<void *>(ea_buffer), ea_length);
}

template <unsigned N>
LIBC_INLINE NTSTATUS open_condrv_absolute(
    HANDLE *out, const WCHAR (&path)[N],
    ACCESS_MASK desired_access = FILE_GENERIC_READ | FILE_GENERIC_WRITE,
    ULONG create_options = FILE_SYNCHRONOUS_IO_NONALERT) {
  UNICODE_STRING name;
  init_unicode(&name, path);

  auto oa = windows::named_internal_oa(&name);

  IO_STATUS_BLOCK iosb = {};
  return ::NtCreateFile(out, desired_access, &oa, &iosb, nullptr, 0,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_OPEN, create_options, nullptr, 0);
}

// Create a ConDrv object relative to a connection or server root. KernelBase's
// launch/attach path uses NtCreateFile(FILE_CREATE) for \Connect, \Input, and
// \Output rather than the NtOpenFile-based Terminal helpers.
template <unsigned N>
LIBC_INLINE NTSTATUS create_condrv_child(
    HANDLE *out, HANDLE root, const WCHAR (&path)[N],
    ACCESS_MASK desired_access =
        FILE_GENERIC_READ | FILE_GENERIC_WRITE | SYNCHRONIZE,
    ULONG create_options = FILE_SYNCHRONOUS_IO_NONALERT,
    bool inherit = true,
    const void *ea_buffer = nullptr, ULONG ea_length = 0) {
  return create_condrv_path_on(out, root, path, desired_access, create_options,
                               inherit, FILE_CREATE, ea_buffer, ea_length);
}

// Open a child handle relative to a ConDrv server handle.
// Matches open-source Terminal CreateClientHandle().
// name: CONDRV_REFERENCE_NAME, CONDRV_INPUT_NAME, or CONDRV_OUTPUT_NAME.
template <unsigned N>
LIBC_INLINE NTSTATUS open_condrv_child(HANDLE *out, HANDLE root,
                                       const WCHAR (&path)[N]) {
  UNICODE_STRING name;
  init_unicode(&name, path);

  auto oa = windows::named_internal_oa(&name, root);

  IO_STATUS_BLOCK iosb = {};
  return ::NtCreateFile(out, FILE_GENERIC_READ | FILE_GENERIC_WRITE, &oa, &iosb,
                        nullptr, 0,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);
}

//===----------------------------------------------------------------------===//
// EA builders
//===----------------------------------------------------------------------===//

LIBC_INLINE void build_condrv_server_ea(
    unsigned char *buffer,
    const CONSOLE_SERVER_MSG &server_msg) {
  auto *ea = reinterpret_cast<FILE_FULL_EA_INFORMATION *>(buffer);
  ea->NextEntryOffset = 0;
  ea->Flags = 0;
  ea->EaNameLength = sizeof(CONDRV_SERVER_EA_NAME) - 1;
  ea->EaValueLength = sizeof(CONSOLE_SERVER_MSG);
  __builtin_memcpy(ea->EaName, CONDRV_SERVER_EA_NAME,
                   sizeof(CONDRV_SERVER_EA_NAME));
  auto *value = buffer + ea_value_offset(ea->EaNameLength);
  __builtin_memcpy(value, &server_msg, sizeof(server_msg));
}

LIBC_INLINE void build_condrv_attach_ea(
    unsigned char *buffer,
    const CONSOLE_SERVER_MSG &server_msg, HANDLE process_id) {
  auto *ea1 = reinterpret_cast<FILE_FULL_EA_INFORMATION *>(buffer);
  ea1->NextEntryOffset = 0;
  ea1->Flags = 0;
  ea1->EaNameLength = sizeof(CONDRV_SERVER_EA_NAME) - 1;
  ea1->EaValueLength = sizeof(CONSOLE_SERVER_MSG);
  __builtin_memcpy(ea1->EaName, CONDRV_SERVER_EA_NAME,
                   sizeof(CONDRV_SERVER_EA_NAME));
  auto *server_value = buffer + ea_value_offset(ea1->EaNameLength);
  __builtin_memcpy(server_value, &server_msg, sizeof(server_msg));
  ea1->NextEntryOffset = align_up_4(CONDRV_SERVER_EA_SIZE);

  auto *ea2 = reinterpret_cast<FILE_FULL_EA_INFORMATION *>(buffer +
                                                           ea1->NextEntryOffset);
  ea2->NextEntryOffset = 0;
  ea2->Flags = 0;
  ea2->EaNameLength = sizeof(CONDRV_ATTACH_EA_NAME) - 1;
  ea2->EaValueLength = sizeof(CD_ATTACH_INFORMATION);
  __builtin_memcpy(ea2->EaName, CONDRV_ATTACH_EA_NAME,
                   sizeof(CONDRV_ATTACH_EA_NAME));

  CD_ATTACH_INFORMATION attach = {process_id};
  auto *value = buffer + ea1->NextEntryOffset + ea_value_offset(ea2->EaNameLength);
  __builtin_memcpy(value, &attach, sizeof(attach));
}

// Open the ConDrv server handle (\Device\ConDrv\Server).
// Matches open-source Terminal CreateServerHandle().
LIBC_INLINE NTSTATUS open_condrv_server(
    HANDLE *out, ACCESS_MASK desired_access = GENERIC_ALL,
    bool inherit = true) {
  UNICODE_STRING name;
  init_unicode(&name, CONDRV_SERVER_PATH);

  auto oa = windows::named_oa(&name, nullptr, inherit);

  IO_STATUS_BLOCK iosb = {};
  return ::NtCreateFile(out, desired_access, &oa, &iosb,
                        nullptr, 0,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_OPEN, 0, nullptr, 0);
}

//===----------------------------------------------------------------------===//
// Forward declarations for out-of-line operations (condrv_ops.cpp)
//===----------------------------------------------------------------------===//

NTSTATUS write_console_bytes(HANDLE object_handle, const void *buffer,
                             DWORD bytes_to_write, bool unicode,
                             DWORD *bytes_written);

NTSTATUS write_console_input_on(
    HANDLE io_target, const CONSOLE_WRITECONSOLEINPUT_REQUEST &request,
    CONSOLE_WRITECONSOLEINPUT_RESULT *result = nullptr);

NTSTATUS
read_console_cooked_on(HANDLE io_target,
                       const CONSOLE_READCONSOLE_REQUEST &request,
                       CONSOLE_READCONSOLE_RESULT *result = nullptr);

NTSTATUS read_console_input_ex(HANDLE object_handle,
                               CONSOLE_INPUT_RECORD *records,
                               DWORD max_records, DWORD *record_count,
                               USHORT flags, bool unicode = true);

NTSTATUS generate_console_ctrl_event(DWORD event_type,
                                     DWORD process_group_id);

NTSTATUS create_condrv_connect(HANDLE *out, HANDLE root,
                               const CONSOLE_SERVER_MSG &server_msg,
                               bool inherit = true);

NTSTATUS create_condrv_client_handles(CONDRV_CLIENT_HANDLES *handles,
                                      HANDLE connection_root,
                                      bool inherit = true);

NTSTATUS open_condrv_client_handles(CONDRV_CLIENT_HANDLES *handles,
                                    HANDLE connection_root,
                                    bool inherit = true);

NTSTATUS launch_condrv_server_with_params(
    HANDLE server_handle, const UNICODE_STRING *image_path,
    const UNICODE_STRING *command_line, PVOID environment = nullptr,
    const UNICODE_STRING *desktop_info = nullptr);

//===----------------------------------------------------------------------===//
// Inline ConDrv handle operations
//===----------------------------------------------------------------------===//

LIBC_INLINE NTSTATUS create_condrv_attach(HANDLE *out, HANDLE process_id,
                                          const CONSOLE_SERVER_MSG &server_msg,
                                          bool inherit = true) {
  auto ea_s = internal::byte_scratch(CONDRV_ATTACH_CHAIN_SIZE);
  if (!ea_s)
    return STATUS_NO_MEMORY;
  auto *ea = reinterpret_cast<unsigned char *>(ea_s.data());
  build_condrv_attach_ea(ea, server_msg, process_id);

  return create_condrv_path_on(out, nullptr, CONDRV_CONNECT_PATH,
                               FILE_GENERIC_READ | FILE_GENERIC_WRITE |
                                   SYNCHRONIZE,
                               FILE_SYNCHRONOUS_IO_NONALERT, inherit,
                               FILE_CREATE, ea, CONDRV_ATTACH_CHAIN_SIZE);
}

LIBC_INLINE NTSTATUS create_condrv_input(HANDLE *out, HANDLE connection_root,
                                         bool inherit = true) {
  return create_condrv_child(out, connection_root, CONDRV_INPUT_NAME,
                             FILE_GENERIC_READ | FILE_GENERIC_WRITE |
                                 SYNCHRONIZE,
                             FILE_SYNCHRONOUS_IO_NONALERT, inherit);
}

LIBC_INLINE NTSTATUS create_condrv_output(HANDLE *out, HANDLE connection_root,
                                          bool inherit = true) {
  return create_condrv_child(out, connection_root, CONDRV_OUTPUT_NAME,
                             FILE_GENERIC_READ | FILE_GENERIC_WRITE |
                                 SYNCHRONIZE,
                             FILE_SYNCHRONOUS_IO_NONALERT, inherit);
}

LIBC_INLINE NTSTATUS open_condrv_input(HANDLE *out, HANDLE connection_root,
                                       bool inherit = true) {
  UNICODE_STRING name;
  init_unicode(&name, CONDRV_INPUT_NAME);

  auto oa = windows::named_oa(&name, connection_root, inherit);

  IO_STATUS_BLOCK iosb = {};
  return ::NtOpenFile(out,
                      FILE_GENERIC_READ | FILE_GENERIC_WRITE | SYNCHRONIZE,
                      &oa, &iosb,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      FILE_SYNCHRONOUS_IO_NONALERT);
}

LIBC_INLINE NTSTATUS open_condrv_output(HANDLE *out, HANDLE connection_root,
                                        bool inherit = true) {
  UNICODE_STRING name;
  init_unicode(&name, CONDRV_OUTPUT_NAME);

  auto oa = windows::named_oa(&name, connection_root, inherit);

  IO_STATUS_BLOCK iosb = {};
  return ::NtOpenFile(out,
                      FILE_GENERIC_READ | FILE_GENERIC_WRITE | SYNCHRONIZE,
                      &oa, &iosb,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      FILE_SYNCHRONOUS_IO_NONALERT);
}

LIBC_INLINE NTSTATUS duplicate_condrv_output_as_error(HANDLE output,
                                                      HANDLE *error,
                                                      bool inherit = true) {
  return ::NtDuplicateObject(NtCurrentProcess(), output, NtCurrentProcess(),
                             error, 0, inherit ? OBJ_INHERIT : 0,
                             DUPLICATE_SAME_ACCESS);
}

LIBC_INLINE void close_condrv_client_handles(CONDRV_CLIENT_HANDLES *handles) {
  if (!handles)
    return;
  if (handles->Error)
    (void)::NtClose(handles->Error);
  if (handles->Output)
    (void)::NtClose(handles->Output);
  if (handles->Input)
    (void)::NtClose(handles->Input);
  if (handles->Connection)
    (void)::NtClose(handles->Connection);
  handles->Error = nullptr;
  handles->Output = nullptr;
  handles->Input = nullptr;
  handles->Connection = nullptr;
}

// Raw launch helper for IOCTL_CONDRV_LAUNCH_SERVER. The decoded
// AllocConsoleWithOptions path feeds a real RTL_USER_PROCESS_PARAMETERS block
// built from the conhost image path and command line.
LIBC_INLINE NTSTATUS launch_condrv_server(HANDLE server_handle,
                                          RTL_USER_PROCESS_PARAMETERS *params) {
  if (!server_handle || !params)
    return STATUS_INVALID_PARAMETER;

  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status =
      ::NtDeviceIoControlFile(server_handle, nullptr, nullptr, nullptr, &iosb,
                              IOCTL_CONDRV_LAUNCH_SERVER, params, params->Length,
                              nullptr, 0);
  if (!NT_SUCCESS(status))
    return status;
  return iosb.Status;
}

LIBC_INLINE NTSTATUS get_condrv_server_pid(HANDLE console_handle,
                                           ULONG64 *server_pid) {
  if (!console_handle || !server_pid)
    return STATUS_INVALID_PARAMETER;

  IO_STATUS_BLOCK iosb = {};
  ULONG64 pid = 0;
  NTSTATUS status = ::NtDeviceIoControlFile(
      console_handle, nullptr, nullptr, nullptr, &iosb,
      IOCTL_CONDRV_GET_SERVER_PID, nullptr, 0, &pid, sizeof(pid));
  if (!NT_SUCCESS(status))
    return status;
  if (!NT_SUCCESS(iosb.Status))
    return iosb.Status;
  *server_pid = pid;
  return iosb.Status;
}

//===----------------------------------------------------------------------===//
// Thin inline forwarders to out-of-line functions
//===----------------------------------------------------------------------===//

LIBC_INLINE NTSTATUS write_console_a(HANDLE object_handle, const CHAR *buffer,
                                     DWORD chars_to_write,
                                     DWORD *chars_written) {
  return write_console_bytes(object_handle, buffer, chars_to_write,
                             /*unicode=*/false, chars_written);
}

LIBC_INLINE NTSTATUS write_console_w(HANDLE object_handle,
                                     const WCHAR *buffer,
                                     DWORD chars_to_write,
                                     DWORD *chars_written) {
  constexpr DWORD MAX_WIDE_CHARS =
      0xFFFFFFFFu / static_cast<DWORD>(sizeof(WCHAR));
  if (chars_written)
    *chars_written = 0;
  if (chars_to_write > MAX_WIDE_CHARS)
    return STATUS_INVALID_PARAMETER;

  DWORD bytes_written = 0;
  NTSTATUS status = write_console_bytes(
      object_handle, buffer,
      chars_to_write * static_cast<DWORD>(sizeof(WCHAR)),
      /*unicode=*/true, chars_written ? &bytes_written : nullptr);
  if (NT_SUCCESS(status) && chars_written)
    *chars_written = bytes_written / static_cast<DWORD>(sizeof(WCHAR));
  return status;
}

LIBC_INLINE NTSTATUS write_console_input(
    const CONSOLE_WRITECONSOLEINPUT_REQUEST &request,
    CONSOLE_WRITECONSOLEINPUT_RESULT *result = nullptr) {
  return write_console_input_on(nullptr, request, result);
}

LIBC_INLINE NTSTATUS
write_console_input_w(HANDLE object_handle,
                      const CONSOLE_INPUT_RECORD *records, ULONG record_count,
                      ULONG *records_written = nullptr, bool append = true) {
  CONSOLE_WRITECONSOLEINPUT_RESULT result = {};
  NTSTATUS status = write_console_input(
      {object_handle, records, record_count, BOOLEAN(1),
       append ? BOOLEAN(1) : BOOLEAN(0)},
      &result);
  if (records_written)
    *records_written = NT_SUCCESS(status) ? result.NumRecords : 0;
  return status;
}

LIBC_INLINE NTSTATUS
write_console_input_a(HANDLE object_handle,
                      const CONSOLE_INPUT_RECORD *records, ULONG record_count,
                      ULONG *records_written = nullptr, bool append = true) {
  CONSOLE_WRITECONSOLEINPUT_RESULT result = {};
  NTSTATUS status = write_console_input(
      {object_handle, records, record_count, BOOLEAN(0),
       append ? BOOLEAN(1) : BOOLEAN(0)},
      &result);
  if (records_written)
    *records_written = NT_SUCCESS(status) ? result.NumRecords : 0;
  return status;
}

LIBC_INLINE NTSTATUS
read_console_cooked(const CONSOLE_READCONSOLE_REQUEST &request,
                    CONSOLE_READCONSOLE_RESULT *result = nullptr) {
  return read_console_cooked_on(nullptr, request, result);
}

LIBC_INLINE NTSTATUS read_console_input(HANDLE object_handle,
                                        CONSOLE_INPUT_RECORD *records,
                                        DWORD max_records,
                                        DWORD *record_count,
                                        bool unicode = true) {
  return read_console_input_ex(object_handle, records, max_records,
                               record_count, 0, unicode);
}

LIBC_INLINE NTSTATUS peek_console_input(HANDLE object_handle,
                                        CONSOLE_INPUT_RECORD *records,
                                        DWORD max_records,
                                        DWORD *record_count,
                                        bool unicode = true) {
  return read_console_input_ex(object_handle, records, max_records,
                               record_count,
                               CONSOLE_READ_NOREMOVE | CONSOLE_READ_NOWAIT,
                               unicode);
}

LIBC_INLINE NTSTATUS flush_console_input(HANDLE object_handle) {
  return issue_console_message(object_handle, OP_FLUSH_INPUT_BUFFER);
}

} // namespace condrv
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_CONDRV_OPS_H
