//===-- Deep probe: IO Ring read on console handles ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Focused test: can IO Ring read from a console input buffer?
//
// Tests:
//   1. Current console + inject keystrokes + IO Ring read
//   2. Raw mode (ENABLE_LINE_INPUT off) + IO Ring read
//   3. Cooked mode (default) + IO Ring read
//   4. Overlapped vs sync ConDrv handle
//   5. NtReadFile baseline for comparison
//
// Writes all results to C:\console_read_log.txt (survives console tear-down).
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/ipc/condrv.h"
#include "test/src/__support/windows/nt_ioring_test_utils.h"

using size_t = decltype(sizeof(0));

// Console APIs not yet replaced with a verified direct ConDrv helper.
extern "C" {
// INPUT_RECORD for synthetic console input injection.
struct KEY_EVENT_RECORD_W {
  BOOL bKeyDown;
  WORD wRepeatCount;
  WORD wVirtualKeyCode;
  WORD wVirtualScanCode;
  WCHAR UnicodeChar;
  DWORD dwControlKeyState;
};

struct INPUT_RECORD_W {
  WORD EventType;
  union {
    KEY_EVENT_RECORD_W KeyEvent;
    char _pad[16]; // other event types we don't use
  } Event;
};

inline constexpr WORD KEY_EVENT = 0x0001;

__declspec(dllimport) BOOL __stdcall WriteConsoleInputW(
    HANDLE hConsoleInput, const INPUT_RECORD_W *lpBuffer, DWORD nLength,
    DWORD *lpNumberOfEventsWritten);
} // extern "C"

// Console mode flags.
inline constexpr DWORD ENABLE_PROCESSED_INPUT = 0x0001;
inline constexpr DWORD ENABLE_LINE_INPUT = 0x0002;
inline constexpr DWORD ENABLE_ECHO_INPUT = 0x0004;
inline constexpr DWORD ENABLE_VIRTUAL_TERMINAL_INPUT = 0x0200;

//===----------------------------------------------------------------------===//
// Log file — writes results to disk independent of console state.
//===----------------------------------------------------------------------===//
static HANDLE log_h = nullptr;
static HANDLE g_console_in = nullptr;  // valid ConDrv input handle
static HANDLE g_console_out = nullptr; // valid ConDrv output handle

static void log_init() {
  // Use temp directory — C:\ root may need admin.
  WCHAR P[256] = {};
  size_t tlen = LIBC_NAMESPACE::test_support::get_temp_path_w(P + 4, 200);
  P[0] = L'\\'; P[1] = L'?'; P[2] = L'?'; P[3] = L'\\';
  // Append filename.
  static constexpr WCHAR FN[] = L"console_read_log.txt";
  for (size_t i = 0; FN[i]; ++i)
    P[4 + tlen + i] = FN[i];
  size_t total = 4 + tlen;
  while (P[total]) ++total;
  UNICODE_STRING us;
  OBJECT_ATTRIBUTES oa;
  LIBC_NAMESPACE::test_support::init_object_attributes(&oa, &us, P, total);
  IO_STATUS_BLOCK iosb = {};
  NtCreateFile(&log_h, FILE_GENERIC_WRITE, &oa, &iosb, nullptr,
               FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OVERWRITE_IF,
               FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_ALERT,
               nullptr, 0);
}

static void L(const char *s) {
  if (!log_h)
    return;
  size_t len = 0;
  while (s[len])
    ++len;
  IO_STATUS_BLOCK iosb = {};
  NtWriteFile(log_h, nullptr, nullptr, nullptr, &iosb, const_cast<char *>(s),
              static_cast<ULONG>(len), nullptr, nullptr);
}

// Log a hex value.
static void Lx(unsigned val) {
  char buf[12] = "0x";
  static constexpr char hex[] = "0123456789ABCDEF";
  for (int i = 7; i >= 0; --i)
    buf[2 + (7 - i)] = hex[(val >> (i * 4)) & 0xF];
  buf[10] = '\0';
  L(buf);
}

// Log a decimal.
static void Ld(int val) {
  if (val < 0) {
    L("-");
    val = -val;
  }
  char buf[12];
  int pos = 0;
  if (val == 0)
    buf[pos++] = '0';
  else {
    char tmp[12];
    int tp = 0;
    while (val > 0) {
      tmp[tp++] = '0' + (val % 10);
      val /= 10;
    }
    for (int i = tp - 1; i >= 0; --i)
      buf[pos++] = tmp[i];
  }
  buf[pos] = '\0';
  L(buf);
}

//===----------------------------------------------------------------------===//
// IO Ring wrapper
//===----------------------------------------------------------------------===//
namespace {
namespace test_support = LIBC_NAMESPACE::test_support;
using Ring = test_support::IoRing;

void log_result(const char *label, Ring::Result &res) {
  L("  ");
  L(label);
  L(": ");
  if (res.timed_out) {
    L("TIMEOUT\n");
  } else if (res.got_cqe) {
    L("CQE hr=");
    Lx(static_cast<unsigned>(res.hr));
    L(" bytes=");
    Ld(static_cast<int>(res.bytes));
    L("\n");
  } else {
    L("NO_CQE\n");
  }
}

// Flush stale input between tests.
void flush_input() {
  if (!g_console_in)
    return;

  char buf[64];
  for (;;) {
    LARGE_INTEGER poll = {};
    NTSTATUS ws = NtWaitForSingleObject(g_console_in, FALSE, &poll);
    if (ws != STATUS_SUCCESS)
      break;

    HANDLE event = nullptr;
    if (!NT_SUCCESS(
            NtCreateEvent(&event, EVENT_MODIFY_STATE | SYNCHRONIZE, nullptr,
                          SynchronizationEvent, FALSE))) {
      break;
    }

    IO_STATUS_BLOCK iosb = {};
    NTSTATUS status = NtReadFile(g_console_in, event, nullptr, nullptr, &iosb,
                                 buf, sizeof(buf), nullptr, nullptr);
    if (status == STATUS_PENDING) {
      LARGE_INTEGER timeout;
      timeout.QuadPart = -50LL * 10000LL;
      ws = NtWaitForSingleObject(event, FALSE, &timeout);
      if (ws == STATUS_TIMEOUT) {
        IO_STATUS_BLOCK cancel = {};
        NtCancelIoFileEx(g_console_in, &iosb, &cancel);
        NtClose(event);
        break;
      }
      status = iosb.Status;
    }

    NtClose(event);
    if (!NT_SUCCESS(status) || iosb.Information == 0)
      break;
  }
}

// Inject keystrokes into console input buffer. Returns total events written.
int inject_keys(HANDLE hin, const WCHAR *text) {
  int total = 0;
  for (size_t i = 0; text[i]; ++i) {
    INPUT_RECORD_W recs[2] = {};
    // Key down.
    recs[0].EventType = KEY_EVENT;
    recs[0].Event.KeyEvent.bKeyDown = TRUE;
    recs[0].Event.KeyEvent.wRepeatCount = 1;
    recs[0].Event.KeyEvent.UnicodeChar = text[i];
    // Key up.
    recs[1].EventType = KEY_EVENT;
    recs[1].Event.KeyEvent.bKeyDown = FALSE;
    recs[1].Event.KeyEvent.wRepeatCount = 1;
    recs[1].Event.KeyEvent.UnicodeChar = text[i];

    DWORD written = 0;
    BOOL ok = WriteConsoleInputW(hin, recs, 2, &written);
    if (ok)
      total += static_cast<int>(written);
    else {
      L("  WriteConsoleInputW FAILED\n");
      return -1;
    }
  }
  return total;
}

bool queue_read(Ring &ring, HANDLE handle, void *buffer, size_t length,
                ULONG_PTR user_data = 1) {
  return ring.push_read(handle, buffer, static_cast<ULONG>(length), 0,
                        user_data, IORING_SQE_FLAG_NONE) != nullptr;
}

} // namespace

//===----------------------------------------------------------------------===//
// Test 1: Current console + inject keystrokes + IO Ring read (cooked mode)
//===----------------------------------------------------------------------===//
static void test_cooked_read() {
  L("\n[TEST 1] Cooked mode: inject keys + IO Ring read\n");
  flush_input();

  HANDLE hin = g_console_in;
  HANDLE hout = g_console_out;

  DWORD mode = 0;
  LIBC_NAMESPACE::condrv::get_console_mode(hin, &mode);
  L("  input mode: ");
  Lx(mode);
  L("\n");

  // Ensure cooked mode (line input + echo).
  LIBC_NAMESPACE::condrv::set_console_mode(hin, ENABLE_PROCESSED_INPUT |
                                                    ENABLE_LINE_INPUT |
                                                    ENABLE_ECHO_INPUT);

  // Inject "hello\r" — the \r completes the line in cooked mode.
  int injected = inject_keys(hin, L"hello\r");
  L("  injected events: ");
  Ld(injected);
  L("\n");

  // Brief delay for condrv to process the input records.
  LARGE_INTEGER delay;
  delay.QuadPart = -200LL * 10000LL; // 200ms
  NtWaitForSingleObject(hin, FALSE, &delay);

  // IO Ring read.
  Ring r;
  char buf[64] = {};
  if (!queue_read(r, hin, buf, sizeof(buf)))
    return;
  auto res = r.submit_and_wait_cancel_on_timeout(2000, 500);
  log_result("cooked ioring read", res);

  if (res.got_cqe && res.hr == test_support::S_OK && res.bytes > 0) {
    L("  data: \"");
    // Print readable portion.
    for (ULONG_PTR i = 0; i < res.bytes && i < 32; ++i) {
      char c = buf[i];
      if (c >= 32 && c < 127) {
        char s[2] = {c, '\0'};
        L(s);
      } else if (c == '\r')
        L("\\r");
      else if (c == '\n')
        L("\\n");
      else
        L(".");
    }
    L("\"\n");
  }
}

//===----------------------------------------------------------------------===//
// Test 2: Raw mode + inject keystrokes + IO Ring read
//===----------------------------------------------------------------------===//
static void test_raw_read() {
  L("\n[TEST 2] Raw mode: inject keys + IO Ring read\n");

  HANDLE hin = ::NtCurrentStandardInput();

  // Set raw mode — no line input, no echo.
  LIBC_NAMESPACE::condrv::set_console_mode(hin, ENABLE_PROCESSED_INPUT);

  DWORD mode = 0;
  LIBC_NAMESPACE::condrv::get_console_mode(hin, &mode);
  L("  input mode: ");
  Lx(mode);
  L("\n");

  // Inject single character.
  inject_keys(hin, L"X");

  LARGE_INTEGER delay;
  delay.QuadPart = -100LL * 10000LL;
  NtWaitForSingleObject(hin, FALSE, &delay);

  Ring r;
  char buf[64] = {};
  if (!queue_read(r, hin, buf, sizeof(buf)))
    return;
  auto res = r.submit_and_wait_cancel_on_timeout(2000, 500);
  log_result("raw ioring read", res);

  if (res.got_cqe && res.hr == test_support::S_OK && res.bytes > 0) {
    L("  data: \"");
    for (ULONG_PTR i = 0; i < res.bytes && i < 16; ++i) {
      char c = buf[i];
      char s[2] = {(c >= 32 && c < 127) ? c : '.', '\0'};
      L(s);
    }
    L("\"\n");
  }
}

//===----------------------------------------------------------------------===//
// Test 3: NtReadFile baseline (cooked mode, injected input)
//===----------------------------------------------------------------------===//
static void test_ntreadfile_baseline() {
  L("\n[TEST 3] NtReadFile baseline (cooked, injected input)\n");
  flush_input();

  HANDLE hin = g_console_in;

  // Cooked mode.
  LIBC_NAMESPACE::condrv::set_console_mode(hin, ENABLE_PROCESSED_INPUT |
                                                    ENABLE_LINE_INPUT |
                                                    ENABLE_ECHO_INPUT);

  // Inject "test\r".
  inject_keys(hin, L"test\r");

  LARGE_INTEGER delay;
  delay.QuadPart = -100LL * 10000LL;
  NtWaitForSingleObject(hin, FALSE, &delay);

  // Overlapped NtReadFile with event.
  HANDLE revt = nullptr;
  NtCreateEvent(&revt, EVENT_MODIFY_STATE | SYNCHRONIZE, nullptr, SynchronizationEvent, FALSE);

  char buf[64] = {};
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status =
      NtReadFile(hin, revt, nullptr, nullptr, &iosb, buf, sizeof(buf),
                 nullptr, nullptr);
  L("  NtReadFile initial: ");
  Lx(static_cast<unsigned>(status));
  L("\n");

  if (status == STATUS_PENDING) {
    LARGE_INTEGER timeout;
    timeout.QuadPart = -2000LL * 10000LL;
    NTSTATUS ws = NtWaitForSingleObject(revt, FALSE, &timeout);
    L("  wait: ");
    Lx(static_cast<unsigned>(ws));
    L("\n");
    if (ws == STATUS_TIMEOUT) {
      IO_STATUS_BLOCK cancel = {};
      NtCancelIoFileEx(hin, &iosb, &cancel);
      NtClose(revt);
      L("  TIMEOUT\n");
      return;
    }
    status = iosb.Status;
  }

  L("  status: ");
  Lx(static_cast<unsigned>(status));
  L(" bytes=");
  Ld(static_cast<int>(iosb.Information));
  L("\n");

  if (NT_SUCCESS(status) && iosb.Information > 0) {
    L("  data: \"");
    for (ULONG_PTR i = 0; i < iosb.Information && i < 32; ++i) {
      char c = buf[i];
      if (c >= 32 && c < 127) {
        char s[2] = {c, '\0'};
        L(s);
      } else if (c == '\r')
        L("\\r");
      else if (c == '\n')
        L("\\n");
      else
        L(".");
    }
    L("\"\n");
  }

  NtClose(revt);
}

//===----------------------------------------------------------------------===//
// Test 4: Direct \Device\ConDrv\Input (overlapped) + inject + IO Ring read
//===----------------------------------------------------------------------===//
static void test_condrv_direct_read() {
  L("\n[TEST 4] Direct ConDrv\\Input (overlapped) + inject + IO Ring read\n");

  // Open ConDrv Input overlapped.
  static constexpr WCHAR PATH[] = L"\\Device\\ConDrv\\Input";
  UNICODE_STRING us;
  OBJECT_ATTRIBUTES oa;
  test_support::init_object_attributes(&oa, &us, PATH,
                                       sizeof(PATH) / sizeof(WCHAR) - 1);

  IO_STATUS_BLOCK iosb = {};
  HANDLE h = nullptr;
  NTSTATUS status = NtOpenFile(&h, FILE_GENERIC_READ, &oa, &iosb,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               FILE_NON_DIRECTORY_FILE);
  L("  NtOpenFile: ");
  Lx(static_cast<unsigned>(status));
  L("\n");
  if (!NT_SUCCESS(status))
    return;

  L("  type: ");
  Ld(static_cast<int>(test_support::query_file_type(h)));
  L("\n");

  // Inject into the console input buffer via the valid console handle.
  inject_keys(g_console_in, L"condrv\r");

  LARGE_INTEGER delay;
  delay.QuadPart = -200LL * 10000LL;
  NtWaitForSingleObject(g_console_in, FALSE, &delay);

  Ring r;
  char buf[64] = {};
  if (!queue_read(r, h, buf, sizeof(buf))) {
    NtClose(h);
    return;
  }
  auto res = r.submit_and_wait_cancel_on_timeout(2000, 500);
  log_result("condrv ioring read", res);

  if (res.got_cqe && res.hr == test_support::S_OK && res.bytes > 0) {
    L("  data: \"");
    for (ULONG_PTR i = 0; i < res.bytes && i < 32; ++i) {
      char c = buf[i];
      if (c >= 32 && c < 127) {
        char s[2] = {c, '\0'};
        L(s);
      } else if (c == '\r')
        L("\\r");
      else if (c == '\n')
        L("\\n");
      else
        L(".");
    }
    L("\"\n");
  }

  NtClose(h);
}

//===----------------------------------------------------------------------===//
// Test 5: Empty console (no input) — should we get EOF or timeout?
//===----------------------------------------------------------------------===//
static void test_empty_console_read() {
  L("\n[TEST 5] Empty console (no injected input) + IO Ring read\n");
  flush_input();

  HANDLE hin = g_console_in;

  // Drain any leftover input.
  // A true input-buffer flush helper would be ideal here.
  // Just skip — we'll inject nothing and see what happens.

  Ring r;
  char buf[64] = {};
  if (!queue_read(r, hin, buf, sizeof(buf)))
    return;
  auto res = r.submit_and_wait_cancel_on_timeout(1000, 500);
  log_result("empty read", res);
}

//===----------------------------------------------------------------------===//
// Test 6: Wait on console handle, then IO Ring read.
// Does waiting for the handle + IO Ring read compose into a blocking read?
//===----------------------------------------------------------------------===//
static void test_wait_then_read() {
  L("\n[TEST 6] Wait-then-read: NtWaitForSingleObject + IO Ring read\n");
  flush_input();

  HANDLE hin = g_console_in;

  // Cooked mode.
  LIBC_NAMESPACE::condrv::set_console_mode(hin, ENABLE_PROCESSED_INPUT |
                                                    ENABLE_LINE_INPUT |
                                                    ENABLE_ECHO_INPUT);

  // Inject a complete line.
  inject_keys(hin, L"waitread\r");

  // Wait for the console handle to signal (input available).
  LARGE_INTEGER timeout;
  timeout.QuadPart = -2000LL * 10000LL; // 2s
  NTSTATUS ws = NtWaitForSingleObject(hin, /*Alertable=*/TRUE, &timeout);
  L("  wait result: ");
  Lx(static_cast<unsigned>(ws));
  L(ws == STATUS_SUCCESS ? " (SIGNALED)\n" :
    ws == STATUS_TIMEOUT ? " (TIMEOUT)\n" : " (OTHER)\n");

  if (ws != STATUS_SUCCESS) {
    L("  SKIP: handle not signaled\n");
    return;
  }

  // Handle is signaled — IO Ring read should get the data.
  Ring r;
  char buf[64] = {};
  if (!queue_read(r, hin, buf, sizeof(buf)))
    return;
  auto res = r.submit_and_wait_cancel_on_timeout(2000, 500);
  log_result("wait+read", res);

  if (res.got_cqe && res.hr == test_support::S_OK && res.bytes > 0) {
    L("  data: \"");
    for (ULONG_PTR i = 0; i < res.bytes && i < 32; ++i) {
      char c = buf[i];
      if (c >= 32 && c < 127) { char s[2] = {c, '\0'}; L(s); }
      else if (c == '\r') L("\\r");
      else if (c == '\n') L("\\n");
      else L(".");
    }
    L("\"\n");
  }
}

//===----------------------------------------------------------------------===//
// Test 7: When does the console handle signal?
// Inject individual keystrokes WITHOUT \r. Does it signal on each keystroke
// (problematic for cooked mode) or only on complete lines?
//===----------------------------------------------------------------------===//
static void test_wait_signal_timing() {
  L("\n[TEST 7] Signal timing: inject partial line, check handle state\n");
  flush_input();

  HANDLE hin = g_console_in;

  // Cooked mode.
  LIBC_NAMESPACE::condrv::set_console_mode(hin, ENABLE_PROCESSED_INPUT |
                                                    ENABLE_LINE_INPUT |
                                                    ENABLE_ECHO_INPUT);

  // Inject partial line (no \r) — just "abc".
  inject_keys(hin, L"abc");

  // Check if handle signals within 200ms.
  LARGE_INTEGER timeout;
  timeout.QuadPart = -200LL * 10000LL;
  NTSTATUS ws = NtWaitForSingleObject(hin, FALSE, &timeout);
  L("  partial line wait: ");
  Lx(static_cast<unsigned>(ws));
  L(ws == STATUS_SUCCESS ? " (SIGNALED — input records exist)\n" :
    ws == STATUS_TIMEOUT ? " (TIMEOUT — no signal)\n" : "\n");

  // Try IO Ring read — should EOF (no complete line).
  {
    Ring r;
    char buf[64] = {};
    if (!queue_read(r, hin, buf, sizeof(buf)))
      return;
    auto res = r.submit_and_wait_cancel_on_timeout(500, 500);
    log_result("partial line read", res);
  }

  // Now inject \r to complete the line.
  inject_keys(hin, L"\r");

  // Wait again.
  timeout.QuadPart = -500LL * 10000LL;
  ws = NtWaitForSingleObject(hin, FALSE, &timeout);
  L("  after \\r wait: ");
  Lx(static_cast<unsigned>(ws));
  L(ws == STATUS_SUCCESS ? " (SIGNALED)\n" : " (TIMEOUT)\n");

  // IO Ring read should now return the complete line.
  {
    Ring r;
    char buf[64] = {};
    if (!queue_read(r, hin, buf, sizeof(buf)))
      return;
    auto res = r.submit_and_wait_cancel_on_timeout(2000, 500);
    log_result("complete line read", res);

    if (res.got_cqe && res.hr == test_support::S_OK && res.bytes > 0) {
      L("  data: \"");
      for (ULONG_PTR i = 0; i < res.bytes && i < 32; ++i) {
        char c = buf[i];
        if (c >= 32 && c < 127) { char s[2] = {c, '\0'}; L(s); }
        else if (c == '\r') L("\\r");
        else if (c == '\n') L("\\n");
        else L(".");
      }
      L("\"\n");
    }
  }
}

//===----------------------------------------------------------------------===//
// Test 8: Universal blocking read pattern.
// Loop: wait on console handle (alertable) → IO Ring read → if EOF, retry.
// This is the candidate pattern for replacing sync NtReadFile.
//===----------------------------------------------------------------------===//
static void test_universal_blocking_read() {
  L("\n[TEST 8] Universal blocking read pattern\n");
  flush_input();

  HANDLE hin = g_console_in;

  // Cooked mode.
  LIBC_NAMESPACE::condrv::set_console_mode(hin, ENABLE_PROCESSED_INPUT |
                                                    ENABLE_LINE_INPUT |
                                                    ENABLE_ECHO_INPUT);

  // Inject partial line, then complete it after 500ms delay (simulating typing).
  inject_keys(hin, L"uni");

  // Simulate delayed Enter via a second inject after a wait.
  // (In real usage, the user types — here we inject synchronously after a gap.)
  LARGE_INTEGER gap;
  gap.QuadPart = -300LL * 10000LL; // 300ms
  // We can't truly delay the inject without threads, so just inject the
  // whole thing and test the wait+retry loop logic.
  inject_keys(hin, L"versal\r");

  // Inject complete line.
  inject_keys(hin, L"universal\r");

  // Single Ring instance — create before waiting.
  Ring r;
  if (!r.ok) {
    L("  ring creation failed\n");
    return;
  }

  // Step 1: Alertable wait for console to have a complete line.
  LARGE_INTEGER timeout;
  timeout.QuadPart = -2000LL * 10000LL;
  NTSTATUS ws = NtWaitForSingleObject(hin, /*Alertable=*/TRUE, &timeout);
  L("  wait: ");
  Lx(static_cast<unsigned>(ws));
  L("\n");
  if (ws != STATUS_SUCCESS) {
    L("  SKIP: not signaled\n");
    return;
  }

  // Step 2: IO Ring read — data should be immediately available.
  char buf[64] = {};
  if (!queue_read(r, hin, buf, sizeof(buf)))
    return;
  auto res = r.submit_and_wait_cancel_on_timeout(2000, 500);
  log_result("universal read", res);

  if (res.got_cqe && res.hr == test_support::S_OK && res.bytes > 0) {
    L("  data: \"");
    for (ULONG_PTR i = 0; i < res.bytes && i < 32; ++i) {
      char c = buf[i];
      if (c >= 32 && c < 127) { char s[2] = {c, '\0'}; L(s); }
      else if (c == '\r') L("\\r");
      else if (c == '\n') L("\\n");
      else L(".");
    }
    L("\"\n");
  }
}

int main() {
  log_init();
  L("=== IO Ring Console Read Deep Probe ===\n");
  L("Opening current ConDrv handles directly.\n");

  // Open real console I/O handles via ConDrv for the current console session.
  auto open_condrv = [](const WCHAR *path, ACCESS_MASK access) -> HANDLE {
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    size_t len = test_support::wide_nul_terminated_length(path);
    test_support::init_object_attributes(&oa, &us, path, len);
    IO_STATUS_BLOCK iosb = {};
    HANDLE h = nullptr;
    // Sync alertable — matches pipe behavior, supports NtReadFile blocking.
    NtCreateFile(&h, access | SYNCHRONIZE, &oa, &iosb, nullptr,
                 FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
                 FILE_OPEN, FILE_SYNCHRONOUS_IO_ALERT, nullptr, 0);
    return h;
  };

  g_console_in = open_condrv(L"\\Device\\ConDrv\\CurrentIn",
                             FILE_GENERIC_READ | FILE_GENERIC_WRITE);
  g_console_out = open_condrv(L"\\Device\\ConDrv\\CurrentOut",
                              FILE_GENERIC_WRITE);
  HANDLE hin = g_console_in;
  HANDLE hout = g_console_out;

  L("  stdin:  handle=");
  Lx(reinterpret_cast<unsigned long long>(hin) & 0xFFFFFFFF);
  DWORD in_type = hin ? test_support::query_file_type(hin) : 0xFFFF;
  L(" type=");
  Ld(static_cast<int>(in_type));
  L("\n  stdout: handle=");
  Lx(reinterpret_cast<unsigned long long>(hout) & 0xFFFFFFFF);
  DWORD out_type = hout ? test_support::query_file_type(hout) : 0xFFFF;
  L(" type=");
  Ld(static_cast<int>(out_type));
  L("\n");

  // Check console mode to verify handle is valid.
  DWORD mode = 0;
  NTSTATUS mode_status = LIBC_NAMESPACE::condrv::get_console_mode(hin, &mode);
  BOOL mode_ok = NT_SUCCESS(mode_status);
  L("  condrv::get_console_mode(stdin): ok=");
  Ld(mode_ok);
  L(" mode=");
  Lx(mode);
  L(" status=");
  Lx(static_cast<unsigned>(mode_status));
  L("\n");

  if (!mode_ok) {
    L("FATAL: Console handles not valid. Cannot test.\n");
    if (hin) NtClose(hin);
    if (hout) NtClose(hout);
    if (log_h) NtClose(log_h);
    return 1;
  }

  // Tests with data — should all succeed.
  test_ntreadfile_baseline();
  test_cooked_read();
  test_wait_then_read();
  test_universal_blocking_read();
  // Tests that may leave pending I/O — run last.
  test_wait_signal_timing();
  test_empty_console_read();
  test_condrv_direct_read();
  test_raw_read();

  L("\n=== Done ===\n");
  if (hin)
    NtClose(hin);
  if (hout)
    NtClose(hout);
  if (log_h)
    NtClose(log_h);
  return 0;
}
