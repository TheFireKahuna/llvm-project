//===-- Test IO Ring behavior on console handles ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Probes IO Ring read/write on console handles to determine what condrv.sys
// supports. Must be run from a real console (not piped/redirected).
//
// Key questions:
//   - Can IO Ring WRITE to stdout/stderr? (the common case)
//   - Can IO Ring READ from stdin? (blocks on input — test with timeout)
//   - Does opening \Device\ConDrv directly change behavior?
//   - What HRESULT does the CQE return on failure?
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/ntdll.h"
#include "test/src/__support/windows/nt_ioring_test_utils.h"

using size_t = decltype(sizeof(0));
extern "C" int printf(const char *, ...);

// Log file handle — printf writes to stdout (console), log() writes to file.
static HANDLE log_handle = nullptr;

static void log_init() {
  static constexpr WCHAR PATH[] =
      L"\\??\\C:\\console_test_log.txt";
  UNICODE_STRING us;
  OBJECT_ATTRIBUTES oa;
  LIBC_NAMESPACE::test_support::init_object_attributes(
      &oa, &us, PATH, sizeof(PATH) / sizeof(WCHAR) - 1);

  IO_STATUS_BLOCK iosb = {};
  NtCreateFile(&log_handle, FILE_GENERIC_WRITE, &oa, &iosb, nullptr,
               FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OVERWRITE_IF,
               FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_ALERT,
               nullptr, 0);
}

static void log(const char *s) {
  if (!log_handle) return;
  size_t len = 0;
  while (s[len]) ++len;
  IO_STATUS_BLOCK iosb = {};
  NtWriteFile(log_handle, nullptr, nullptr, nullptr, &iosb,
              const_cast<char *>(s), static_cast<ULONG>(len),
              nullptr, nullptr);
}

// tee_printf: writes to both stdout (printf) and the log file.
// Use instead of printf for all test output.
#define tprintf(...) do { printf(__VA_ARGS__); } while(0)
// After each printf block, also write the formatted result to log.
// For simplicity, just use printf for output and read the log at the end.
// We'll add explicit log() calls for key results.

static int test_count = 0;

namespace {
namespace test_support = LIBC_NAMESPACE::test_support;
using Ring = test_support::IoRing;

const char *hr_name(HRESULT hr) {
  if (hr == test_support::S_OK) return "S_OK";
  if (hr == static_cast<HRESULT>(0x80004005L)) return "E_FAIL";
  if (hr == static_cast<HRESULT>(0x80070006L)) return "E_HANDLE";
  if (hr == static_cast<HRESULT>(0x80070026L)) return "EOF";
  if (hr == static_cast<HRESULT>(0x80070057L)) return "E_INVALIDARG";
  if (hr == static_cast<HRESULT>(0x8007006DL)) return "BROKEN_PIPE";
  if (hr == static_cast<HRESULT>(0x800703E3L)) return "CANCELLED";
  if (hr == static_cast<HRESULT>(0x800700E8L)) return "NO_DATA";
  return "???";
}

// Format result into buf, write to both console and log.
void print_result(const char *label, Ring::Result &res) {
  char buf[256];
  if (res.timed_out) {
    printf("  %s: TIMEOUT (no CQE within deadline)\n", label);
    log("TIMEOUT: "); log(label); log("\n");
  } else if (res.got_cqe) {
    printf("  %s: CQE hr=0x%08X (%s) bytes=%d\n", label,
           static_cast<unsigned>(res.hr), hr_name(res.hr),
           static_cast<int>(res.bytes));
    log("CQE: "); log(label); log(" hr="); log(hr_name(res.hr)); log("\n");
  } else {
    printf("  %s: no CQE (hr=0x%08X)\n", label,
           static_cast<unsigned>(res.hr));
    log("NO_CQE: "); log(label); log("\n");
  }
}

} // namespace

//===----------------------------------------------------------------------===//
// Detect handle types
//===----------------------------------------------------------------------===//
static void test_handle_types() {
  printf("[INFO] Handle types\n");
  struct { HANDLE hnd; const char *name; } handles[] = {
    {NtCurrentStandardInput(),  "stdin"},
    {NtCurrentStandardOutput(), "stdout"},
    {NtCurrentStandardError(),  "stderr"},
  };
  for (auto &h : handles) {
    HANDLE hnd = h.hnd;
    if (!test_support::is_valid_handle(hnd)) {
      printf("  %s: not available\n", h.name);
      continue;
    }
    DWORD ftype = test_support::query_file_type(hnd);
    const char *tname = "UNKNOWN";
    if (ftype == test_support::FILE_TYPE_CHAR) tname = "CHAR (console)";
    else if (ftype == test_support::FILE_TYPE_PIPE) tname = "PIPE (redirected)";
    else if (ftype == test_support::FILE_TYPE_DISK) tname = "DISK (redirected to file)";
    printf("  %s: handle=%p type=%s\n", h.name,
           static_cast<void *>(hnd), tname);
    log("  "); log(h.name); log(": "); log(tname); log("\n");
  }
}

//===----------------------------------------------------------------------===//
// 1. Console WRITE via IO Ring (stdout)
//===----------------------------------------------------------------------===//
static void test_console_write_stdout() {
  printf("[TEST] Console write via IO Ring (stdout)\n");
  ++test_count;

  HANDLE h = ::NtCurrentStandardOutput();
  if (!test_support::is_valid_handle(h)) {
    printf("  SKIP: no stdout\n");
    return;
  }
  if (test_support::query_file_type(h) != test_support::FILE_TYPE_CHAR) {
    printf("  SKIP: stdout not a console (redirected)\n");
    return;
  }

  Ring r;
  char msg[] = "  [IO Ring wrote this to stdout]\n";
  if (!r.push_write(h, msg, sizeof(msg) - 1, 0, 1, IORING_SQE_FLAG_NONE)) {
    printf("  SKIP: ring queue full\n");
    return;
  }
  auto res = r.submit_and_wait_cancel_on_timeout(2000);
  print_result("stdout write", res);
}

//===----------------------------------------------------------------------===//
// 2. Console WRITE via IO Ring (stderr)
//===----------------------------------------------------------------------===//
static void test_console_write_stderr() {
  printf("[TEST] Console write via IO Ring (stderr)\n");
  ++test_count;

  HANDLE h = ::NtCurrentStandardError();
  if (!test_support::is_valid_handle(h)) {
    printf("  SKIP: no stderr\n");
    return;
  }
  if (test_support::query_file_type(h) != test_support::FILE_TYPE_CHAR) {
    printf("  SKIP: stderr not a console\n");
    return;
  }

  Ring r;
  char msg[] = "  [IO Ring wrote this to stderr]\n";
  if (!r.push_write(h, msg, sizeof(msg) - 1, 0, 1, IORING_SQE_FLAG_NONE)) {
    printf("  SKIP: ring queue full\n");
    return;
  }
  auto res = r.submit_and_wait_cancel_on_timeout(2000);
  print_result("stderr write", res);
}

//===----------------------------------------------------------------------===//
// 3. Console READ via IO Ring (stdin) — short timeout, will likely block
//===----------------------------------------------------------------------===//
static void test_console_read_stdin() {
  printf("[TEST] Console read via IO Ring (stdin, 500ms timeout)\n");
  ++test_count;

  HANDLE h = ::NtCurrentStandardInput();
  if (!test_support::is_valid_handle(h)) {
    printf("  SKIP: no stdin\n");
    return;
  }
  if (test_support::query_file_type(h) != test_support::FILE_TYPE_CHAR) {
    printf("  SKIP: stdin not a console\n");
    return;
  }

  Ring r;
  char buf[64] = {};
  if (!r.push_read(h, buf, sizeof(buf), 0, 1, IORING_SQE_FLAG_NONE)) {
    printf("  SKIP: ring queue full\n");
    return;
  }
  auto res = r.submit_and_wait_cancel_on_timeout(500);
  print_result("stdin read", res);

  if (res.timed_out)
    printf("  (expected: console read blocks waiting for input)\n");
}

//===----------------------------------------------------------------------===//
// 4. Open \Device\ConDrv\Output directly and test write
//===----------------------------------------------------------------------===//
static void test_condrv_output_direct() {
  printf("[TEST] Direct \\Device\\ConDrv\\Output write via IO Ring\n");
  ++test_count;

  static constexpr WCHAR PATH[] = L"\\Device\\ConDrv\\Output";
  UNICODE_STRING us;
  OBJECT_ATTRIBUTES oa;
  test_support::init_object_attributes(&oa, &us, PATH,
                                       sizeof(PATH) / sizeof(WCHAR) - 1);

  IO_STATUS_BLOCK iosb = {};
  HANDLE h = nullptr;
  NTSTATUS status = NtOpenFile(
      &h, FILE_GENERIC_WRITE, &oa, &iosb,
      FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_NON_DIRECTORY_FILE);
  if (!NT_SUCCESS(status)) {
    printf("  SKIP: NtOpenFile failed 0x%08X\n",
           static_cast<unsigned>(status));
    return;
  }

  DWORD ftype = test_support::query_file_type(h);
  printf("  handle=%p type=%lu\n", static_cast<void *>(h), ftype);

  Ring r;
  char msg[] = "  [IO Ring via ConDrv\\Output]\n";
  if (!r.push_write(h, msg, sizeof(msg) - 1, 0, 1, IORING_SQE_FLAG_NONE)) {
    printf("  SKIP: ring queue full\n");
    NtClose(h);
    return;
  }
  auto res = r.submit_and_wait_cancel_on_timeout(2000);
  print_result("condrv write", res);

  NtClose(h);
}

//===----------------------------------------------------------------------===//
// 5. Open \Device\ConDrv\Input directly and test read
//===----------------------------------------------------------------------===//
static void test_condrv_input_direct() {
  printf("[TEST] Direct \\Device\\ConDrv\\Input read via IO Ring (500ms)\n");
  ++test_count;

  static constexpr WCHAR PATH[] = L"\\Device\\ConDrv\\Input";
  UNICODE_STRING us;
  OBJECT_ATTRIBUTES oa;
  test_support::init_object_attributes(&oa, &us, PATH,
                                       sizeof(PATH) / sizeof(WCHAR) - 1);

  IO_STATUS_BLOCK iosb = {};
  HANDLE h = nullptr;
  NTSTATUS status = NtOpenFile(
      &h, FILE_GENERIC_READ, &oa, &iosb,
      FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_NON_DIRECTORY_FILE);
  if (!NT_SUCCESS(status)) {
    printf("  SKIP: NtOpenFile failed 0x%08X\n",
           static_cast<unsigned>(status));
    return;
  }

  DWORD ftype = test_support::query_file_type(h);
  printf("  handle=%p type=%lu\n", static_cast<void *>(h), ftype);

  Ring r;
  char buf[64] = {};
  if (!r.push_read(h, buf, sizeof(buf), 0, 1, IORING_SQE_FLAG_NONE)) {
    printf("  SKIP: ring queue full\n");
    NtClose(h);
    return;
  }
  auto res = r.submit_and_wait_cancel_on_timeout(500);
  print_result("condrv read", res);

  NtClose(h);
}

//===----------------------------------------------------------------------===//
// 6. NtWriteFile directly on console (baseline comparison)
//===----------------------------------------------------------------------===//
static void test_nt_write_console() {
  printf("[TEST] NtWriteFile on console stdout (baseline)\n");
  ++test_count;

  HANDLE h = ::NtCurrentStandardOutput();
  if (!test_support::is_valid_handle(h) ||
      test_support::query_file_type(h) != test_support::FILE_TYPE_CHAR) {
    printf("  SKIP: no console stdout\n");
    return;
  }

  IO_STATUS_BLOCK iosb = {};
  char msg[] = "  [NtWriteFile wrote this to stdout]\n";
  NTSTATUS status = NtWriteFile(h, nullptr, nullptr, nullptr, &iosb,
                                msg, sizeof(msg) - 1, nullptr, nullptr);
  printf("  NtWriteFile: 0x%08X bytes=%d\n",
         static_cast<unsigned>(status),
         static_cast<int>(iosb.Information));
}

//===----------------------------------------------------------------------===//
// 7. test_support::write_handle on console (baseline comparison)
//===----------------------------------------------------------------------===//
static void test_writefile_console() {
  printf("[TEST] NT write helper on console stdout (baseline)\n");
  ++test_count;

  HANDLE h = ::NtCurrentStandardOutput();
  if (!test_support::is_valid_handle(h) ||
      test_support::query_file_type(h) != test_support::FILE_TYPE_CHAR) {
    printf("  SKIP: no console stdout\n");
    return;
  }

  DWORD written = 0;
  char msg[] = "  [test_support::write_handle wrote this to stdout]\n";
  bool ok = test_support::write_handle(h, msg, sizeof(msg) - 1, &written);
  printf("  write_handle: ok=%d written=%lu\n", ok ? 1 : 0, written);
}

//===----------------------------------------------------------------------===//
// 8. IO Ring write with overlapped console handle
//===----------------------------------------------------------------------===//
static void test_console_write_overlapped() {
  printf("[TEST] Console write via IO Ring (overlapped open)\n");
  ++test_count;

  // Open the current console's output with overlapped flag.
  static constexpr WCHAR PATH[] = L"\\Device\\ConDrv\\CurrentOut";
  UNICODE_STRING us;
  OBJECT_ATTRIBUTES oa;
  test_support::init_object_attributes(&oa, &us, PATH,
                                       sizeof(PATH) / sizeof(WCHAR) - 1);

  IO_STATUS_BLOCK iosb = {};
  HANDLE h = nullptr;
  // Try overlapped (no FILE_SYNCHRONOUS_IO_ALERT).
  NTSTATUS status = NtOpenFile(
      &h, FILE_GENERIC_WRITE, &oa, &iosb,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      FILE_NON_DIRECTORY_FILE);
  if (!NT_SUCCESS(status)) {
    // Try alternate path.
    static constexpr WCHAR PATH2[] = L"\\Device\\ConDrv\\Output";
    test_support::init_object_attributes(&oa, &us, PATH2,
                                         sizeof(PATH2) / sizeof(WCHAR) - 1);
    IO_STATUS_BLOCK iosb2 = {};
    status = NtOpenFile(&h, FILE_GENERIC_WRITE, &oa, &iosb2,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        FILE_NON_DIRECTORY_FILE);
  }
  if (!NT_SUCCESS(status)) {
    printf("  SKIP: NtOpenFile failed 0x%08X\n",
           static_cast<unsigned>(status));
    return;
  }

  printf("  handle=%p type=%lu\n", static_cast<void *>(h),
         test_support::query_file_type(h));

  Ring r;
  char msg[] = "  [IO Ring overlapped console write]\n";
  if (!r.push_write(h, msg, sizeof(msg) - 1, 0, 1, IORING_SQE_FLAG_NONE)) {
    printf("  SKIP: ring queue full\n");
    NtClose(h);
    return;
  }
  auto res = r.submit_and_wait_cancel_on_timeout(2000);
  print_result("ovl write", res);

  NtClose(h);
}

//===----------------------------------------------------------------------===//
// 9. Current console handles + test all operations on real console handles
//===----------------------------------------------------------------------===//
static void test_alloc_console() {
  printf("[TEST] Current console: IO Ring on real console handles\n");
  log("=== Current console test ===\n");
  ++test_count;

  HANDLE hout = ::NtCurrentStandardOutput();
  HANDLE hin = ::NtCurrentStandardInput();
  HANDLE herr = ::NtCurrentStandardError();
  if (!test_support::is_valid_handle(hout) ||
      !test_support::is_valid_handle(hin)) {
    printf("  SKIP: no current console handles\n");
    log("SKIP: no current console handles\n");
    return;
  }

  DWORD out_type = test_support::query_file_type(hout);
  DWORD in_type = test_support::query_file_type(hin);

  log(out_type == test_support::FILE_TYPE_CHAR ? "stdout: CHAR\n"
                                               : "stdout: not CHAR\n");
  log(in_type == test_support::FILE_TYPE_CHAR ? "stdin: CHAR\n"
                                              : "stdin: not CHAR\n");

  // --- Test write to stdout ---
  if (out_type == test_support::FILE_TYPE_CHAR) {
    Ring r;
    char msg[] = "[Current console IO Ring write test]\r\n";
    if (r.push_write(hout, msg, sizeof(msg) - 1, 0, 1,
                     IORING_SQE_FLAG_NONE)) {
      auto res = r.submit_and_wait_cancel_on_timeout(2000);
      print_result("current stdout write", res);
    }
  }

  // --- Test write to stderr ---
  if (test_support::query_file_type(herr) == test_support::FILE_TYPE_CHAR) {
    Ring r;
    char msg[] = "[Current console IO Ring stderr write]\r\n";
    if (r.push_write(herr, msg, sizeof(msg) - 1, 0, 1,
                     IORING_SQE_FLAG_NONE)) {
      auto res = r.submit_and_wait_cancel_on_timeout(2000);
      print_result("current stderr write", res);
    }
  }

  // --- Test read from stdin (short timeout) ---
  if (in_type == test_support::FILE_TYPE_CHAR) {
    Ring r;
    char buf[64] = {};
    if (r.push_read(hin, buf, sizeof(buf), 0, 1, IORING_SQE_FLAG_NONE)) {
      auto res = r.submit_and_wait_cancel_on_timeout(500);
      print_result("current stdin read", res);
    }
  }
}

int main() {
  log_init();
  log("=== IO Ring Console Probe ===\n");
  printf("=== IO Ring Console Probe ===\n\n");

  test_handle_types();
  printf("\n");
  test_nt_write_console();
  printf("\n");
  test_writefile_console();
  printf("\n");
  test_console_write_stdout();
  printf("\n");
  test_console_write_stderr();
  printf("\n");
  test_console_write_overlapped();
  printf("\n");
  test_condrv_output_direct();
  printf("\n");
  test_console_read_stdin();
  printf("\n");
  test_condrv_input_direct();
  printf("\n");
  test_alloc_console();

  printf("\n=== Done (%d tests) ===\n", test_count);
  log("=== Done ===\n");
  if (log_handle)
    NtClose(log_handle);
  return 0;
}
