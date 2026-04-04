//===-- ConDrv type definitions and constants --------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Structure definitions, constants, enums, and static_assert ABI validation
// for the ConDrv console driver interface.
//
// Device paths and connection flow verified against the open-source
// Windows Terminal (microsoft/terminal) DeviceHandle.cpp:
//   \Device\ConDrv\Server   -- server connection
//   \Reference              -- keepalive handle (relative to server)
//   \Input                  -- stdin (relative to server)
//   \Output                 -- stdout/stderr (relative to server)
//
// IOCTL_CONDRV_ISSUE_USER_IO verified in practice (console mode,
// codepage, and WriteConsole operations all work through it).
// The broader IOCTL table and the user-defined buffer envelope come from
// the public Windows Terminal header plus local decode work.
//
// Decoded message structures and operation codes are in the reference
// file: windows-itanium-reference/nt_headers/condrv_decode_effort.h
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_CONDRV_TYPES_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_CONDRV_TYPES_H

#include "src/__support/OSUtil/windows/nt/nt_file.h"
#include "src/__support/OSUtil/windows/nt/nt_security_types.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace condrv {

//===----------------------------------------------------------------------===//
// ConDrv device paths (verified: open-source Terminal DeviceHandle.cpp)
//===----------------------------------------------------------------------===//

inline constexpr const WCHAR CONDRV_SERVER_PATH[] = u"\\Device\\ConDrv\\Server";
inline constexpr const WCHAR CONDRV_CONNECT_PATH[] =
    u"\\Device\\ConDrv\\Connect";
inline constexpr const WCHAR CONDRV_CONNECT_NAME[] = u"\\Connect";
inline constexpr const WCHAR CONDRV_REFERENCE_NAME[] = u"\\Reference";
inline constexpr const WCHAR CONDRV_INPUT_NAME[] = u"\\Input";
inline constexpr const WCHAR CONDRV_OUTPUT_NAME[] = u"\\Output";
inline constexpr const WCHAR CONDRV_CURRENT_INPUT_PATH[] =
    u"\\Device\\ConDrv\\CurrentIn";
inline constexpr const WCHAR CONDRV_CURRENT_OUTPUT_PATH[] =
    u"\\Device\\ConDrv\\CurrentOut";
inline constexpr const CHAR CONDRV_SERVER_EA_NAME[] = "server";
inline constexpr const CHAR CONDRV_ATTACH_EA_NAME[] = "attach";

//===----------------------------------------------------------------------===//
// ConDrv IOCTL -- verified in use
//===----------------------------------------------------------------------===//

inline constexpr ULONG FILE_ANY_ACCESS = 0;
inline constexpr ULONG METHOD_BUFFERED = 0;
inline constexpr ULONG METHOD_OUT_DIRECT = 2;
inline constexpr ULONG METHOD_NEITHER = 3;

LIBC_INLINE constexpr ULONG CONDRV_CONTROL_CODE(ULONG function, ULONG method) {
  return (FILE_DEVICE_CONSOLE << 16) | (FILE_ANY_ACCESS << 14) |
         (function << 2) | method;
}

inline constexpr ULONG IOCTL_CONDRV_READ_IO =
    CONDRV_CONTROL_CODE(1, METHOD_OUT_DIRECT);
inline constexpr ULONG IOCTL_CONDRV_COMPLETE_IO =
    CONDRV_CONTROL_CODE(2, METHOD_NEITHER);
inline constexpr ULONG IOCTL_CONDRV_READ_INPUT =
    CONDRV_CONTROL_CODE(3, METHOD_NEITHER);
inline constexpr ULONG IOCTL_CONDRV_WRITE_OUTPUT =
    CONDRV_CONTROL_CODE(4, METHOD_NEITHER);
// Master IOCTL for console API dispatch. Operation codes are embedded
// in the message payload.
inline constexpr ULONG IOCTL_CONDRV_ISSUE_USER_IO =
    CONDRV_CONTROL_CODE(5, METHOD_OUT_DIRECT);
inline constexpr ULONG IOCTL_CONDRV_DISCONNECT_PIPE =
    CONDRV_CONTROL_CODE(6, METHOD_NEITHER);
inline constexpr ULONG IOCTL_CONDRV_SET_SERVER_INFORMATION =
    CONDRV_CONTROL_CODE(7, METHOD_NEITHER);
inline constexpr ULONG IOCTL_CONDRV_GET_SERVER_PID =
    CONDRV_CONTROL_CODE(8, METHOD_NEITHER);
inline constexpr ULONG IOCTL_CONDRV_GET_DISPLAY_SIZE =
    CONDRV_CONTROL_CODE(9, METHOD_NEITHER);
inline constexpr ULONG IOCTL_CONDRV_UPDATE_DISPLAY =
    CONDRV_CONTROL_CODE(10, METHOD_NEITHER);
inline constexpr ULONG IOCTL_CONDRV_SET_CURSOR =
    CONDRV_CONTROL_CODE(11, METHOD_NEITHER);
inline constexpr ULONG IOCTL_CONDRV_ALLOW_VIA_UIACCESS =
    CONDRV_CONTROL_CODE(12, METHOD_NEITHER);
inline constexpr ULONG IOCTL_CONDRV_LAUNCH_SERVER =
    CONDRV_CONTROL_CODE(13, METHOD_NEITHER);
inline constexpr ULONG IOCTL_CONDRV_GET_FONT_SIZE =
    CONDRV_CONTROL_CODE(14, METHOD_NEITHER);

// Console input mode flags.
inline constexpr DWORD ENABLE_PROCESSED_INPUT = 0x0001;
inline constexpr DWORD ENABLE_LINE_INPUT = 0x0002;
inline constexpr DWORD ENABLE_ECHO_INPUT = 0x0004;
inline constexpr DWORD ENABLE_WINDOW_INPUT = 0x0008;
inline constexpr DWORD ENABLE_MOUSE_INPUT = 0x0010;
inline constexpr DWORD ENABLE_INSERT_MODE = 0x0020;
inline constexpr DWORD ENABLE_QUICK_EDIT_MODE = 0x0040;
inline constexpr DWORD ENABLE_EXTENDED_FLAGS = 0x0080;
inline constexpr DWORD ENABLE_AUTO_POSITION = 0x0100;
inline constexpr DWORD ENABLE_VIRTUAL_TERMINAL_INPUT = 0x0200;

// Console output mode flags.
inline constexpr DWORD ENABLE_PROCESSED_OUTPUT = 0x0001;
inline constexpr DWORD ENABLE_WRAP_AT_EOL_OUTPUT = 0x0002;
inline constexpr DWORD ENABLE_VIRTUAL_TERMINAL_PROCESSING = 0x0004;
inline constexpr DWORD DISABLE_NEWLINE_AUTO_RETURN = 0x0008;
inline constexpr DWORD ENABLE_LVB_GRID_WORLDWIDE = 0x0010;

//===----------------------------------------------------------------------===//
// Public/server ConDrv ABI from Terminal's condrv.h/conmsgl1.h
//===----------------------------------------------------------------------===//

inline constexpr ULONG CONSOLE_IO_CONNECT = 0x01;
inline constexpr ULONG CONSOLE_IO_DISCONNECT = 0x02;
inline constexpr ULONG CONSOLE_IO_CREATE_OBJECT = 0x03;
inline constexpr ULONG CONSOLE_IO_CLOSE_OBJECT = 0x04;
inline constexpr ULONG CONSOLE_IO_RAW_WRITE = 0x05;
inline constexpr ULONG CONSOLE_IO_RAW_READ = 0x06;
inline constexpr ULONG CONSOLE_IO_USER_DEFINED = 0x07;
inline constexpr ULONG CONSOLE_IO_RAW_FLUSH = 0x08;

inline constexpr ULONG CD_IO_OBJECT_TYPE_CURRENT_INPUT = 0x01;
inline constexpr ULONG CD_IO_OBJECT_TYPE_CURRENT_OUTPUT = 0x02;
inline constexpr ULONG CD_IO_OBJECT_TYPE_NEW_OUTPUT = 0x03;
inline constexpr ULONG CD_IO_OBJECT_TYPE_GENERIC = 0x04;

struct CD_IO_DESCRIPTOR {
  LUID Identifier;
  ULONG_PTR Process;
  ULONG_PTR Object;
  ULONG Function;
  ULONG InputSize;
  ULONG OutputSize;
  ULONG Reserved;
};

struct CD_CREATE_OBJECT_INFORMATION {
  ULONG ObjectType;
  ULONG ShareMode;
  ACCESS_MASK DesiredAccess;
};

struct CD_CONNECTION_INFORMATION {
  ULONG_PTR Process;
  ULONG_PTR Input;
  ULONG_PTR Output;
};

struct CD_IO_BUFFER {
  ULONG Size;
  void *Buffer;
};

struct CD_IO_BUFFER_DESCRIPTOR {
  void *Data;
  ULONG Size;
  ULONG Offset;
};

struct CD_IO_COMPLETE {
  LUID Identifier;
  IO_STATUS_BLOCK IoStatus;
  CD_IO_BUFFER_DESCRIPTOR Write;
};

struct CD_IO_OPERATION {
  LUID Identifier;
  CD_IO_BUFFER_DESCRIPTOR Buffer;
};

struct CD_IO_SERVER_INFORMATION {
  HANDLE InputAvailableEvent;
};

template <unsigned NumBuffers> struct CD_USER_DEFINED_IO_STACK {
  HANDLE Client;
  ULONG InputCount;
  ULONG OutputCount;
  CD_IO_BUFFER Buffers[NumBuffers];
};

struct CD_IO_DISPLAY_SIZE {
  ULONG Width;
  ULONG Height;
};

struct CD_IO_FONT_SIZE {
  ULONG Width;
  ULONG Height;
};

inline constexpr USHORT CONSOLE_READ_NOREMOVE = 0x0001;
inline constexpr USHORT CONSOLE_READ_NOWAIT = 0x0002;
inline constexpr USHORT CONSOLE_READ_VALID =
    CONSOLE_READ_NOREMOVE | CONSOLE_READ_NOWAIT;

inline constexpr USHORT KEY_EVENT = 0x0001;
inline constexpr USHORT MOUSE_EVENT = 0x0002;
inline constexpr USHORT WINDOW_BUFFER_SIZE_EVENT = 0x0004;
inline constexpr USHORT MENU_EVENT = 0x0008;
inline constexpr USHORT FOCUS_EVENT = 0x0010;

LIBC_INLINE constexpr ULONG CONSOLE_FIRST_API_NUMBER(ULONG layer) {
  return layer << 24;
}

inline constexpr ULONG API_NUMBER_GET_CONSOLE_CP = CONSOLE_FIRST_API_NUMBER(1);
inline constexpr ULONG API_NUMBER_GET_CONSOLE_MODE =
    API_NUMBER_GET_CONSOLE_CP + 1;
inline constexpr ULONG API_NUMBER_SET_CONSOLE_MODE =
    API_NUMBER_GET_CONSOLE_CP + 2;
inline constexpr ULONG API_NUMBER_GET_NUMBER_OF_INPUT_EVENTS =
    API_NUMBER_GET_CONSOLE_CP + 3;
inline constexpr ULONG API_NUMBER_GET_CONSOLE_INPUT =
    API_NUMBER_GET_CONSOLE_CP + 4;
inline constexpr ULONG API_NUMBER_READ_CONSOLE = API_NUMBER_GET_CONSOLE_CP + 5;
inline constexpr ULONG API_NUMBER_WRITE_CONSOLE = API_NUMBER_GET_CONSOLE_CP + 6;
inline constexpr ULONG API_NUMBER_NOTIFY_LAST_CLOSE =
    API_NUMBER_GET_CONSOLE_CP + 7;
inline constexpr ULONG API_NUMBER_GET_LANG_ID = API_NUMBER_GET_CONSOLE_CP + 8;
inline constexpr ULONG API_NUMBER_MAP_BITMAP = API_NUMBER_GET_CONSOLE_CP + 9;

inline constexpr ULONG API_NUMBER_GENERATE_CTRL_EVENT =
    CONSOLE_FIRST_API_NUMBER(2) + 1;
inline constexpr ULONG API_NUMBER_FLUSH_INPUT_BUFFER =
    CONSOLE_FIRST_API_NUMBER(2) + 3;
inline constexpr ULONG API_NUMBER_SET_CONSOLE_CP =
    CONSOLE_FIRST_API_NUMBER(2) + 4;
inline constexpr ULONG API_NUMBER_GET_SCREEN_BUFFER_INFO =
    CONSOLE_FIRST_API_NUMBER(2) + 7;
inline constexpr ULONG API_NUMBER_SET_SCREEN_BUFFER_INFO =
    CONSOLE_FIRST_API_NUMBER(2) + 8;
inline constexpr ULONG API_NUMBER_WRITE_CONSOLE_INPUT =
    CONSOLE_FIRST_API_NUMBER(2) + 0x10;

// The public typed console API-number namespace comes from conmsgl1.h /
// conmsgl2.h. Live Win11 24H2 KernelBase disassembly confirms the native
// fixed-packet transport reuses that namespace; the earlier local
// 0x01000007-9 "input op" decode was wrong. The aliases below are limited to
// values directly observed in KernelBase call sites:
//   0x01000000 = GetConsoleCP / GetConsoleOutputCP (Output byte selects view)
//   0x01000003 = GetNumberOfConsoleInputEvents
//   0x01000004 = GetConsoleInput (peek/read selected by Flags)
//   0x01000008 = GetConsoleLangId
//   0x02000004 = SetConsoleCP / SetConsoleOutputCP (Output byte selects view)
//   0x02000007 = GetConsoleScreenBufferInfoEx
//   0x02000008 = SetConsoleScreenBufferInfoEx
inline constexpr ULONG OP_GET_CONSOLE_CP = API_NUMBER_GET_CONSOLE_CP;
inline constexpr ULONG OP_GET_CONSOLE_MODE = API_NUMBER_GET_CONSOLE_MODE;
inline constexpr ULONG OP_SET_CONSOLE_MODE = API_NUMBER_SET_CONSOLE_MODE;
inline constexpr ULONG OP_GET_NUMBER_OF_INPUT_EVENTS =
    API_NUMBER_GET_NUMBER_OF_INPUT_EVENTS;
inline constexpr ULONG OP_GET_CONSOLE_INPUT = API_NUMBER_GET_CONSOLE_INPUT;
inline constexpr ULONG OP_READ_CONSOLE = API_NUMBER_READ_CONSOLE;
inline constexpr ULONG OP_WRITE_CONSOLE = API_NUMBER_WRITE_CONSOLE;
inline constexpr ULONG OP_GET_CONSOLE_LANG_ID = API_NUMBER_GET_LANG_ID;
inline constexpr ULONG OP_GENERATE_CTRL_EVENT =
    API_NUMBER_GENERATE_CTRL_EVENT;
inline constexpr ULONG OP_FLUSH_INPUT_BUFFER = API_NUMBER_FLUSH_INPUT_BUFFER;
inline constexpr ULONG OP_SET_CONSOLE_CP = API_NUMBER_SET_CONSOLE_CP;
inline constexpr ULONG OP_GET_SCREEN_BUFFER_INFO =
    API_NUMBER_GET_SCREEN_BUFFER_INFO;
inline constexpr ULONG OP_SET_SCREEN_BUFFER_INFO =
    API_NUMBER_SET_SCREEN_BUFFER_INFO;
inline constexpr ULONG OP_WRITE_CONSOLE_INPUT =
    API_NUMBER_WRITE_CONSOLE_INPUT;

struct CONSOLE_MSG_HEADER {
  ULONG ApiNumber;
  ULONG ApiDescriptorSize;
};

struct CONSOLE_GETCP_MSG {
  ULONG CodePage;
  BOOLEAN Output;
  UCHAR Padding[3];
};
using CONSOLE_SETCP_MSG = CONSOLE_GETCP_MSG;

struct CONSOLE_LANGID_MSG {
  LANGID LangId;
};

struct CONSOLE_MODE_MSG {
  ULONG Mode;
};

struct CONSOLE_GETNUMBEROFINPUTEVENTS_MSG {
  ULONG ReadyEvents;
};

struct CONSOLE_GETCONSOLEINPUT_MSG {
  ULONG NumRecords;
  USHORT Flags;
  BOOLEAN Unicode;
  UCHAR Padding;
};

// COORD is defined in nt_types.h (included transitively via nt_file.h).
using ::COORD;

struct CONSOLE_SMALL_RECT {
  SHORT Left;
  SHORT Top;
  SHORT Right;
  SHORT Bottom;
};

union CONSOLE_KEY_CHAR {
  WCHAR UnicodeChar;
  CHAR AsciiChar;
};

struct CONSOLE_KEY_EVENT_RECORD {
  BOOL KeyDown;
  USHORT RepeatCount;
  USHORT VirtualKeyCode;
  USHORT VirtualScanCode;
  CONSOLE_KEY_CHAR Character;
  DWORD ControlKeyState;
};

struct CONSOLE_MOUSE_EVENT_RECORD {
  COORD MousePosition;
  DWORD ButtonState;
  DWORD ControlKeyState;
  DWORD EventFlags;
};

struct CONSOLE_WINDOW_BUFFER_SIZE_RECORD {
  COORD Size;
};

struct CONSOLE_MENU_EVENT_RECORD {
  UINT CommandId;
};

struct CONSOLE_FOCUS_EVENT_RECORD {
  BOOL SetFocus;
};

union CONSOLE_INPUT_EVENT {
  CONSOLE_KEY_EVENT_RECORD KeyEvent;
  CONSOLE_MOUSE_EVENT_RECORD MouseEvent;
  CONSOLE_WINDOW_BUFFER_SIZE_RECORD WindowBufferSizeEvent;
  CONSOLE_MENU_EVENT_RECORD MenuEvent;
  CONSOLE_FOCUS_EVENT_RECORD FocusEvent;
};

struct CONSOLE_INPUT_RECORD {
  USHORT EventType;
  USHORT Reserved;
  CONSOLE_INPUT_EVENT Event;
};

struct CONSOLE_READCONSOLE_MSG {
  BOOLEAN Unicode;
  BOOLEAN ProcessControlZ;
  USHORT ExeNameLength;
  ULONG InitialNumBytes;
  ULONG CtrlWakeupMask;
  ULONG ControlKeyState;
  ULONG NumBytes;
};

struct CONSOLE_WRITECONSOLE_MSG {
  ULONG NumBytes;
  BOOLEAN Unicode;
  UCHAR Padding[3];
};

struct CONSOLE_WRITECONSOLEINPUT_MSG {
  ULONG NumRecords;
  BOOLEAN Unicode;
  BOOLEAN Append;
  UCHAR Padding[2];
};

struct CONSOLE_SCREENBUFFERINFO_MSG {
  COORD Size;
  COORD CursorPosition;
  COORD ScrollPosition;
  USHORT Attributes;
  COORD CurrentWindowSize;
  COORD MaximumWindowSize;
  USHORT PopupAttributes;
  BOOLEAN FullscreenSupported;
  UCHAR Padding[3];
  ULONG ColorTable[16];
};

struct CONSOLE_SCREEN_BUFFER_INFO_EX {
  ULONG cbSize;
  COORD dwSize;
  COORD dwCursorPosition;
  USHORT wAttributes;
  CONSOLE_SMALL_RECT srWindow;
  COORD dwMaximumWindowSize;
  USHORT wPopupAttributes;
  BOOL bFullscreenSupported;
  ULONG ColorTable[16];
};

struct CONSOLE_SERVER_MSG {
  ULONG IconId;
  ULONG HotKey;
  ULONG StartupFlags;
  USHORT FillAttribute;
  USHORT ShowWindow;
  COORD ScreenBufferSize;
  COORD WindowSize;
  COORD WindowOrigin;
  ULONG ProcessGroupId;
  BOOLEAN ConsoleApp;
  BOOLEAN WindowVisible;
  USHORT TitleLength;
  WCHAR Title[261];
  USHORT ApplicationNameLength;
  WCHAR ApplicationName[128];
  USHORT CurrentDirectoryLength;
  WCHAR CurrentDirectory[261];
};

struct CD_ATTACH_INFORMATION {
  HANDLE ProcessId;
};

struct CD_ATTACH_INFORMATION64 {
  ULONG64 ProcessId;
};

struct CONDRV_CLIENT_HANDLES {
  HANDLE Connection = nullptr;
  HANDLE Input = nullptr;
  HANDLE Output = nullptr;
  HANDLE Error = nullptr;
};

static_assert(sizeof(CONSOLE_SERVER_MSG) == 0x53C,
              "CONSOLE_SERVER_MSG must match the public ConDrv ABI");
static_assert(sizeof(CD_ATTACH_INFORMATION) == 8,
              "CD_ATTACH_INFORMATION must match the public ConDrv ABI");
static_assert(sizeof(CD_ATTACH_INFORMATION64) == 8,
              "CD_ATTACH_INFORMATION64 must match the public ConDrv ABI");

//===----------------------------------------------------------------------===//
// Verified native client packet for console mode operations
//===----------------------------------------------------------------------===//

// KernelBase's current GetConsoleMode/SetConsoleMode path does not use the
// typed Terminal/user-defined message ABI above. It sends a fixed 0x30-byte
// message to IOCTL_CONDRV_ISSUE_USER_IO with pointers into a packed payload.
struct CD_CLIENT_MESSAGE {
  // When the IOCTL target is the PEB ConsoleHandle, this selects the input or
  // output child object. On direct child-handle targets (CONIN$/CONOUT$), live
  // probes show the driver ignores this field entirely and routes by the IOCTL
  // target even if object_handle is garbage.
  void *object_handle;
  // These two fields are canonically 1/1 in traced KernelBase mode/count/ctrl
  // call sites. On the mode path, probes show input_count must be 1, while
  // output_count gates the copy-back channel: 0 suppresses copy/echo, 1
  // enables it, and >1 is rejected. Keep the names descriptive rather than
  // normative because the broader fixed-packet family is not fully decoded.
  ULONG input_count;
  ULONG output_count;
  ULONG total_message_size; // sizeof(header) + payload for fixed 0x30 calls.
  ULONG pad1;
  // Points to a contiguous {CONSOLE_MSG_HEADER, payload}. On the mode path this
  // is a real input pointer: NULL/garbage fault, and shifted +4/+8 misaligns
  // the header enough for the driver to reject the packet.
  void *message_ptr;
  // Canonical payload byte size. For mode get/set, the driver copies
  // min(data_size, 4) bytes of the mode value to data_ptr. data_ptr is only
  // probed when output_count == 1 and data_size > 0; data_size = 0 suppresses
  // the copy without failing the underlying get/set.
  ULONG data_size;
  ULONG pad2;
  // For mode operations, Get writes the returned mode here and Set echoes the
  // applied mode here. Set reads the desired mode from message_ptr + 8 and
  // only writes back through data_ptr.
  void *data_ptr;
};

// KernelBase uses a typed 8-byte header in both the public user-defined
// transport and the fixed native packet transport.
using CD_CLIENT_HEADER = CONSOLE_MSG_HEADER;
using CD_CLIENT_OPERATION_HEADER = CD_CLIENT_HEADER;

struct CD_CLIENT_MODE_OPERATION {
  CD_CLIENT_OPERATION_HEADER op;
  DWORD value;
};

// Fixed 0x40-byte GetConsoleInput packet verified against live 24H2
// KernelBase call sites and runtime probes.
struct CD_GETCONSOLEINPUT_MESSAGE {
  void *object_handle;
  ULONG input_count;
  ULONG output_count;
  ULONG request_message_size;
  ULONG pad1;
  void *request_message_ptr;
  ULONG result_header_size;
  ULONG pad2;
  void *result_header_ptr;
  ULONG record_buffer_size;
  ULONG pad3;
  void *record_buffer_ptr;
};

static_assert(sizeof(CONSOLE_GETCONSOLEINPUT_MSG) == 0x08,
              "CONSOLE_GETCONSOLEINPUT_MSG must match ConDrv ABI");
static_assert(sizeof(CONSOLE_GETCP_MSG) == 0x08,
              "CONSOLE_GETCP_MSG must match ConDrv ABI");
static_assert(sizeof(CONSOLE_LANGID_MSG) == 0x02,
              "CONSOLE_LANGID_MSG must match ConDrv ABI");
static_assert(sizeof(CONSOLE_READCONSOLE_MSG) == 0x14,
              "CONSOLE_READCONSOLE_MSG must match ConDrv ABI");
static_assert(sizeof(CONSOLE_SCREENBUFFERINFO_MSG) == 0x5C,
              "CONSOLE_SCREENBUFFERINFO_MSG must match ConDrv ABI");
static_assert(sizeof(CONSOLE_SCREEN_BUFFER_INFO_EX) == 0x60,
              "CONSOLE_SCREEN_BUFFER_INFO_EX must match Win32 ABI");
static_assert(sizeof(CONSOLE_WRITECONSOLE_MSG) == 0x08,
              "CONSOLE_WRITECONSOLE_MSG must match ConDrv ABI");
static_assert(sizeof(CONSOLE_WRITECONSOLEINPUT_MSG) == 0x08,
              "CONSOLE_WRITECONSOLEINPUT_MSG must match ConDrv ABI");
static_assert(sizeof(CONSOLE_KEY_EVENT_RECORD) == 0x10,
              "CONSOLE_KEY_EVENT_RECORD must match Win32 ABI");
static_assert(sizeof(CONSOLE_MOUSE_EVENT_RECORD) == 0x10,
              "CONSOLE_MOUSE_EVENT_RECORD must match Win32 ABI");
static_assert(sizeof(CONSOLE_INPUT_RECORD) == 0x14,
              "CONSOLE_INPUT_RECORD must match Win32 ABI");
static_assert(sizeof(CD_GETCONSOLEINPUT_MESSAGE) == 0x40,
              "CD_GETCONSOLEINPUT_MESSAGE must match KernelBase packet size");

using CD_WRITECONSOLE_USER_IO = CD_USER_DEFINED_IO_STACK<3>;
static_assert(sizeof(CD_WRITECONSOLE_USER_IO) == 0x40,
              "CD_WRITECONSOLE_USER_IO must match KernelBase packet size");

using CD_READCONSOLE_USER_IO = CD_USER_DEFINED_IO_STACK<5>;
static_assert(sizeof(CD_READCONSOLE_USER_IO) == 0x60,
              "CD_READCONSOLE_USER_IO must match KernelBase packet size");

using CD_WRITECONSOLEINPUT_USER_IO = CD_USER_DEFINED_IO_STACK<3>;
static_assert(sizeof(CD_WRITECONSOLEINPUT_USER_IO) == 0x40,
              "CD_WRITECONSOLEINPUT_USER_IO must match KernelBase packet size");

//===----------------------------------------------------------------------===//
// Arithmetic helpers needed by EA size constants
//===----------------------------------------------------------------------===//

LIBC_INLINE constexpr ULONG ea_value_offset(ULONG name_len) {
  return static_cast<ULONG>(
             __builtin_offsetof(FILE_FULL_EA_INFORMATION, EaName)) +
         name_len + 1;
}

LIBC_INLINE constexpr ULONG ea_entry_size(ULONG name_len, ULONG value_len) {
  return ea_value_offset(name_len) + value_len;
}

LIBC_INLINE constexpr ULONG align_up_4(ULONG value) {
  return (value + 3u) & ~3u;
}

inline constexpr ULONG CONDRV_SERVER_EA_SIZE =
    ea_entry_size(sizeof(CONDRV_SERVER_EA_NAME) - 1,
                  sizeof(CONSOLE_SERVER_MSG));
inline constexpr ULONG CONDRV_ATTACH_EA_SIZE =
    ea_entry_size(sizeof(CONDRV_ATTACH_EA_NAME) - 1,
                  sizeof(CD_ATTACH_INFORMATION));
inline constexpr ULONG CONDRV_ATTACH_CHAIN_SIZE =
    align_up_4(CONDRV_SERVER_EA_SIZE) + CONDRV_ATTACH_EA_SIZE;

static_assert(CONDRV_SERVER_EA_SIZE == 0x54B,
              "server EA size must match the decoded KernelBase packet");

//===----------------------------------------------------------------------===//
// Request/result structs used by out-of-line operations
//===----------------------------------------------------------------------===//

struct CONSOLE_WRITECONSOLEINPUT_REQUEST {
  HANDLE ObjectHandle;
  const CONSOLE_INPUT_RECORD *Records;
  ULONG RecordCount;
  BOOLEAN Unicode;
  BOOLEAN Append;
};

struct CONSOLE_WRITECONSOLEINPUT_RESULT {
  ULONG NumRecords;
};

struct CONSOLE_READCONSOLE_REQUEST {
  HANDLE ObjectHandle;
  const WCHAR *ExeName;
  USHORT ExeNameLength;
  const void *SeedBuffer;
  ULONG SeedBufferSize;
  void *OutputBuffer;
  ULONG OutputBufferSize;
  ULONG CtrlWakeupMask;
  BOOLEAN Unicode;
  BOOLEAN ProcessControlZ;
};

struct CONSOLE_READCONSOLE_RESULT {
  ULONG NumBytes;
  ULONG ControlKeyState;
};

struct CONSOLE_GENERATE_CTRL_EVENT_MSG {
  DWORD EventType;
  DWORD ProcessGroupId;
};

} // namespace condrv
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_CONDRV_TYPES_H
