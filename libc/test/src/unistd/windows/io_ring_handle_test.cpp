//===-- Test IO Ring behavior across handle types --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Standalone test — probes IO Ring against every handle type and operation
// that pipes need: read, write, flush, EOF, broken-pipe, and cancel.
//
// Build (link with test_stubs.o + ntdll.lib + kernelbase.lib + kernel32.lib):
//   clang -c ... io_ring_handle_test.cpp -o test.o
//   lld-link -entry:mainCRTStartup -subsystem:console ...
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/ntdll.h"
#include "test/src/__support/windows/nt_ioring_test_utils.h"

using size_t = decltype(sizeof(0));
extern "C" int printf(const char *, ...);

static int test_failures = 0;
static int test_passes = 0;
static int pipe_counter = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (cond) {                                                                \
      ++test_passes;                                                           \
      printf("  PASS: %s\n", msg);                                             \
    } else {                                                                   \
      printf("  FAIL: %s (line %d)\n", msg, __LINE__);                         \
      ++test_failures;                                                         \
    }                                                                          \
  } while (0)

#define CHECK_EQ(a, b, msg) CHECK((a) == (b), msg)

namespace {
namespace test_support = LIBC_NAMESPACE::test_support;
using Ring = test_support::IoRing;
using test_support::S_OK;

//===----------------------------------------------------------------------===//
// OA / path helpers
//===----------------------------------------------------------------------===//
static bool build_temp_path(const WCHAR *filename, WCHAR *out,
                            size_t out_cap) {
  size_t len = test_support::get_temp_path_w(out + 4, out_cap - 16);
  if (len == 0)
    return false;
  out[0] = L'\\'; out[1] = L'?'; out[2] = L'?'; out[3] = L'\\';
  size_t pos = 4 + len;
  for (size_t i = 0; filename[i]; ++i)
    out[pos++] = filename[i];
  out[pos] = L'\0';
  return true;
}

//===----------------------------------------------------------------------===//
// Pipe factory — creates an overlapped pipe pair with unique name.
//===----------------------------------------------------------------------===//
struct PipePair {
  HANDLE rd = nullptr;
  HANDLE wr = nullptr;
  bool ok = false;

  PipePair() {
    // Build unique name.
    WCHAR name[80] = L"\\Device\\NamedPipe\\llvm-libc-ioring-";
    size_t pos = test_support::wide_nul_terminated_length(name);
    // Append counter as decimal.
    int c = pipe_counter++;
    if (c == 0) {
      name[pos++] = L'0';
    } else {
      WCHAR tmp[12];
      int tpos = 0;
      while (c > 0) { tmp[tpos++] = L'0' + (c % 10); c /= 10; }
      for (int i = tpos - 1; i >= 0; --i) name[pos++] = tmp[i];
    }
    name[pos] = L'\0';

    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    test_support::init_object_attributes(&oa, &us, name, pos);

    IO_STATUS_BLOCK iosb = {};
    LARGE_INTEGER timeout;
    timeout.QuadPart = -500000000LL;

    // Server (read end) — overlapped (no FILE_SYNCHRONOUS_IO_ALERT).
    NTSTATUS status = NtCreateNamedPipeFile(
        &rd, SYNCHRONIZE | FILE_READ_DATA | FILE_WRITE_ATTRIBUTES,
        &oa, &iosb,
        FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_CREATE,
        0 /* overlapped */,
        FILE_PIPE_BYTE_STREAM_TYPE, FILE_PIPE_BYTE_STREAM_MODE,
        FILE_PIPE_QUEUE_OPERATION, 1, 4096, 4096, &timeout);
    if (!NT_SUCCESS(status))
      return;

    // Client (write end) — also overlapped.
    IO_STATUS_BLOCK iosb2 = {};
    status = NtOpenFile(
        &wr, SYNCHRONIZE | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES,
        &oa, &iosb2,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(status)) {
      NtClose(rd);
      rd = nullptr;
      return;
    }
    ok = true;
  }

  ~PipePair() {
    if (rd) NtClose(rd);
    if (wr) NtClose(wr);
  }
};

} // namespace

//===----------------------------------------------------------------------===//
// 1. Disk file baseline — read
//===----------------------------------------------------------------------===//
static void test_disk_read() {
  printf("[TEST] Disk: IO Ring read\n");

  WCHAR path_buf[512];
  if (!build_temp_path(L"ioring_handle_test.tmp", path_buf, 512)) {
    printf("  SKIP: could not build temp path\n");
    return;
  }

  UNICODE_STRING us;
  OBJECT_ATTRIBUTES oa;
  test_support::init_object_attributes(
      &oa, &us, path_buf, test_support::wide_nul_terminated_length(path_buf));

  // Write with sync handle.
  IO_STATUS_BLOCK iosb = {};
  HANDLE wh = nullptr;
  NtCreateFile(&wh, FILE_GENERIC_WRITE, &oa, &iosb, nullptr,
               FILE_ATTRIBUTE_NORMAL,
               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
               FILE_OVERWRITE_IF,
               FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_ALERT,
               nullptr, 0);
  if (!wh) { printf("  SKIP: create failed\n"); return; }

  IO_STATUS_BLOCK wiosb = {};
  char data[] = "disk read test data";
  NtWriteFile(wh, nullptr, nullptr, nullptr, &wiosb, data, sizeof(data) - 1,
              nullptr, nullptr);
  NtClose(wh);

  // Reopen overlapped for IO Ring.
  IO_STATUS_BLOCK riosb = {};
  HANDLE h = nullptr;
  test_support::init_object_attributes(
      &oa, &us, path_buf, test_support::wide_nul_terminated_length(path_buf));
  NtCreateFile(&h, FILE_GENERIC_READ, &oa, &riosb, nullptr,
               FILE_ATTRIBUTE_NORMAL,
               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
               FILE_OPEN, FILE_NON_DIRECTORY_FILE, nullptr, 0);

  Ring r;
  char buf[64] = {};
  auto *sqe = r.push_read(h, buf, sizeof(buf), 0, 1, IORING_SQE_FLAG_NONE);
  CHECK(sqe != nullptr, "disk read: queued SQE");
  if (!sqe) {
    NtClose(h);
    return;
  }
  auto res = r.submit_and_wait();
  CHECK(res.got_cqe && res.hr == S_OK, "disk read: CQE success");
  CHECK(res.bytes == sizeof(data) - 1, "disk read: correct byte count");

  NtClose(h);

  // Cleanup.
  IO_STATUS_BLOCK diosb = {};
  HANDLE del = nullptr;
  test_support::init_object_attributes(
      &oa, &us, path_buf, test_support::wide_nul_terminated_length(path_buf));
  NtCreateFile(&del, DELETE_ACCESS | SYNCHRONIZE, &oa, &diosb, nullptr,
               FILE_ATTRIBUTE_NORMAL, FILE_SHARE_DELETE, FILE_OPEN,
               FILE_DELETE_ON_CLOSE | FILE_NON_DIRECTORY_FILE, nullptr, 0);
  if (del) NtClose(del);
}

//===----------------------------------------------------------------------===//
// 2. Pipe: IO Ring read
//===----------------------------------------------------------------------===//
static void test_pipe_read() {
  printf("[TEST] Pipe: IO Ring read\n");

  PipePair pp;
  if (!pp.ok) { printf("  SKIP: pipe creation failed\n"); return; }

  // Write data via NtWriteFile on the write end.
  IO_STATUS_BLOCK wiosb = {};
  HANDLE wevt = nullptr;
  NtCreateEvent(&wevt, EVENT_MODIFY_STATE | SYNCHRONIZE, nullptr, SynchronizationEvent, FALSE);
  char data[] = "pipe read test data!";
  NtWriteFile(pp.wr, wevt, nullptr, nullptr, &wiosb, data, sizeof(data) - 1,
              nullptr, nullptr);
  LARGE_INTEGER wt;
  wt.QuadPart = -1000LL * 10000LL;
  NtWaitForSingleObject(wevt, FALSE, &wt);
  NtClose(wevt);

  // IO Ring read on the read end.
  Ring r;
  char buf[64] = {};
  auto *sqe =
      r.push_read(pp.rd, buf, sizeof(buf), 0, 1, IORING_SQE_FLAG_NONE);
  CHECK(sqe != nullptr, "pipe read: queued SQE");
  if (!sqe)
    return;
  auto res = r.submit_and_wait();

  CHECK(res.got_cqe, "pipe read: got CQE");
  CHECK_EQ(res.hr, S_OK, "pipe read: S_OK");
  CHECK(res.bytes == sizeof(data) - 1, "pipe read: correct byte count");

  bool data_match = true;
  for (size_t i = 0; i < sizeof(data) - 1; ++i)
    if (buf[i] != data[i]) data_match = false;
  CHECK(data_match, "pipe read: data matches");
}

//===----------------------------------------------------------------------===//
// 3. Pipe: IO Ring write
//===----------------------------------------------------------------------===//
static void test_pipe_write() {
  printf("[TEST] Pipe: IO Ring write\n");

  PipePair pp;
  if (!pp.ok) { printf("  SKIP: pipe creation failed\n"); return; }

  char data[] = "pipe write test!!!";
  Ring r;
  auto *sqe =
      r.push_write(pp.wr, data, sizeof(data) - 1, 0, 1,
                   IORING_SQE_FLAG_NONE);
  CHECK(sqe != nullptr, "pipe write: queued SQE");
  if (!sqe)
    return;
  auto res = r.submit_and_wait();

  CHECK(res.got_cqe, "pipe write: got CQE");
  CHECK_EQ(res.hr, S_OK, "pipe write: S_OK");
  CHECK(res.bytes == sizeof(data) - 1, "pipe write: correct byte count");

  // Verify by reading back with NtReadFile.
  char buf[64] = {};
  IO_STATUS_BLOCK riosb = {};
  HANDLE revt = nullptr;
  NtCreateEvent(&revt, EVENT_MODIFY_STATE | SYNCHRONIZE, nullptr, SynchronizationEvent, FALSE);
  NtReadFile(pp.rd, revt, nullptr, nullptr, &riosb, buf, sizeof(buf),
             nullptr, nullptr);
  LARGE_INTEGER rt;
  rt.QuadPart = -1000LL * 10000LL;
  NtWaitForSingleObject(revt, FALSE, &rt);
  NtClose(revt);

  bool data_match = true;
  for (size_t i = 0; i < sizeof(data) - 1; ++i)
    if (buf[i] != data[i]) data_match = false;
  CHECK(data_match, "pipe write: readback matches");
}

//===----------------------------------------------------------------------===//
// 4. Pipe: IO Ring flush — deep diagnostic
//===----------------------------------------------------------------------===//

// Test BuildIoRingFlushFile with a specific mode on a pipe handle.
// Returns: 0=no CQE (timeout), 1=CQE with success, -1=CQE with failure.
static int try_pipe_flush_mode(HANDLE h, ULONG mode, const char *label) {
  Ring r;
  if (!r.ok) {
    printf("    %s: ring creation failed\n", label);
    return -1;
  }

  auto *sqe = r.push_flush(h, 1, mode, IORING_SQE_FLAG_NONE);
  printf("    %s: push_flush = %s\n", label, sqe ? "queued" : "full");
  if (!sqe)
    return -1;

  NTSTATUS submit_status = r.submit();
  printf("    %s: submit = 0x%08X\n", label,
         static_cast<unsigned>(submit_status));
  if (!NT_SUCCESS(submit_status))
    return -1;

  // Fast path: check for immediate CQE (no wait).
  NT_IORING_CQE cqe = {};
  if (r.pop_cqe(&cqe)) {
    printf("    %s: immediate CQE! hr=0x%08X bytes=%d\n",
           label, static_cast<unsigned>(cqe.ResultCode),
           static_cast<int>(cqe.Information));
    return test_support::hresult_failed(cqe.ResultCode) ? -1 : 1;
  }

  // Wait with increasing timeouts: 50ms, 200ms, 500ms.
  int timeouts[] = {50, 200, 500};
  for (int i = 0; i < 3; ++i) {
    LARGE_INTEGER timeout;
    timeout.QuadPart = -static_cast<long long>(timeouts[i]) * 10000LL;
    NTSTATUS ws = NtWaitForSingleObject(r.event, FALSE, &timeout);
    printf("    %s: wait %dms -> 0x%08X\n",
           label, timeouts[i], static_cast<unsigned>(ws));

    if (r.pop_cqe(&cqe)) {
      printf("    %s: CQE after %dms: hr=0x%08X bytes=%d\n",
             label, timeouts[i], static_cast<unsigned>(cqe.ResultCode),
             static_cast<int>(cqe.Information));
      return test_support::hresult_failed(cqe.ResultCode) ? -1 : 1;
    }
  }

  printf("    %s: NO CQE after 750ms total\n", label);
  return 0;
}

static void test_pipe_flush() {
  printf("[TEST] Pipe: IO Ring flush (all modes)\n");

  // --- Part A: Direct NtFlushBuffersFileEx on pipe ---
  // NOTE: NtFlushBuffersFile on a pipe with unread data HANGS — npfs.sys
  // blocks until the reader drains the buffer. We test two cases:
  //   (a) empty pipe (nothing written)
  //   (b) drained pipe (written then read back)
  {
    printf("\n  Part A: Direct NT flush on pipe\n");

    // (a) Empty pipe — flush should return immediately.
    {
      printf("    --- empty pipe ---\n");
      PipePair pp;
      if (!pp.ok) { printf("  SKIP\n"); return; }

      IO_STATUS_BLOCK fiosb = {};
      NTSTATUS status = NtFlushBuffersFile(pp.wr, &fiosb);
      printf("    NtFlushBuffersFile:           0x%08X\n",
             static_cast<unsigned>(status));

      IO_STATUS_BLOCK fiosb2 = {};
      status = NtFlushBuffersFileEx(pp.wr, FLUSH_FLAGS_FILE_NORMAL,
                                    nullptr, 0, &fiosb2);
      printf("    NtFlushBuffersFileEx(NORMAL): 0x%08X\n",
             static_cast<unsigned>(status));

      IO_STATUS_BLOCK fiosb3 = {};
      status = NtFlushBuffersFileEx(pp.wr, FLUSH_FLAGS_FILE_DATA_ONLY,
                                    nullptr, 0, &fiosb3);
      printf("    NtFlushBuffersFileEx(DATA):   0x%08X\n",
             static_cast<unsigned>(status));

      IO_STATUS_BLOCK fiosb4 = {};
      status = NtFlushBuffersFileEx(pp.wr, FLUSH_FLAGS_FILE_DATA_SYNC_ONLY,
                                    nullptr, 0, &fiosb4);
      printf("    NtFlushBuffersFileEx(DSYNC):  0x%08X\n",
             static_cast<unsigned>(status));

      IO_STATUS_BLOCK fiosb5 = {};
      status = NtFlushBuffersFileEx(pp.wr, FLUSH_FLAGS_NO_SYNC,
                                    nullptr, 0, &fiosb5);
      printf("    NtFlushBuffersFileEx(NOSYNC): 0x%08X\n",
             static_cast<unsigned>(status));
    }

    // (b) Drained pipe — write then read, then flush.
    {
      printf("    --- drained pipe ---\n");
      PipePair pp;
      if (!pp.ok) { printf("  SKIP\n"); return; }

      // Write.
      IO_STATUS_BLOCK wiosb = {};
      HANDLE wevt = nullptr;
      NtCreateEvent(&wevt, EVENT_MODIFY_STATE | SYNCHRONIZE, nullptr, SynchronizationEvent,
                    FALSE);
      char data[] = "drain me";
      NtWriteFile(pp.wr, wevt, nullptr, nullptr, &wiosb, data,
                  sizeof(data) - 1, nullptr, nullptr);
      LARGE_INTEGER wt;
      wt.QuadPart = -1000LL * 10000LL;
      NtWaitForSingleObject(wevt, FALSE, &wt);
      NtClose(wevt);

      // Read (drain).
      char buf[64] = {};
      IO_STATUS_BLOCK riosb = {};
      HANDLE revt = nullptr;
      NtCreateEvent(&revt, EVENT_MODIFY_STATE | SYNCHRONIZE, nullptr, SynchronizationEvent,
                    FALSE);
      NtReadFile(pp.rd, revt, nullptr, nullptr, &riosb, buf, sizeof(buf),
                 nullptr, nullptr);
      LARGE_INTEGER rt;
      rt.QuadPart = -1000LL * 10000LL;
      NtWaitForSingleObject(revt, FALSE, &rt);
      NtClose(revt);

      // Now flush on drained pipe.
      IO_STATUS_BLOCK fiosb = {};
      NTSTATUS status = NtFlushBuffersFile(pp.wr, &fiosb);
      printf("    NtFlushBuffersFile:           0x%08X\n",
             static_cast<unsigned>(status));

      IO_STATUS_BLOCK fiosb2 = {};
      status = NtFlushBuffersFileEx(pp.wr, FLUSH_FLAGS_FILE_NORMAL,
                                    nullptr, 0, &fiosb2);
      printf("    NtFlushBuffersFileEx(NORMAL): 0x%08X\n",
             static_cast<unsigned>(status));
    }
  }

  // --- Part B: IO Ring flush with each mode on pipe ---
  {
    printf("\n  Part B: IO Ring flush modes on pipe\n");
    PipePair pp;
    if (!pp.ok) { printf("  SKIP: pipe creation failed\n"); return; }

    // Write data.
    IO_STATUS_BLOCK wiosb = {};
    HANDLE wevt = nullptr;
    NtCreateEvent(&wevt, EVENT_MODIFY_STATE | SYNCHRONIZE, nullptr, SynchronizationEvent,
                  FALSE);
    char data[] = "flush test data";
    NtWriteFile(pp.wr, wevt, nullptr, nullptr, &wiosb, data, sizeof(data) - 1,
                nullptr, nullptr);
    LARGE_INTEGER wt;
    wt.QuadPart = -1000LL * 10000LL;
    NtWaitForSingleObject(wevt, FALSE, &wt);
    NtClose(wevt);

    try_pipe_flush_mode(pp.wr, FLUSH_FLAGS_FILE_NORMAL, "DEFAULT");
    try_pipe_flush_mode(pp.wr, FLUSH_FLAGS_FILE_DATA_ONLY, "DATA");
    try_pipe_flush_mode(pp.wr, FLUSH_FLAGS_NO_SYNC, "NO_SYNC");
    try_pipe_flush_mode(pp.wr, FLUSH_FLAGS_FILE_DATA_SYNC_ONLY, "MIN_META");
  }

  // --- Part C: IO Ring flush on disk (baseline) ---
  {
    printf("\n  Part C: IO Ring flush on disk (baseline)\n");

    WCHAR path_buf[512];
    if (!build_temp_path(L"ioring_flush_test.tmp", path_buf, 512)) {
      printf("    SKIP: path build failed\n");
      return;
    }

    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    test_support::init_object_attributes(
        &oa, &us, path_buf, test_support::wide_nul_terminated_length(path_buf));

    IO_STATUS_BLOCK iosb = {};
    HANDLE h = nullptr;
    NtCreateFile(&h, FILE_GENERIC_WRITE, &oa, &iosb, nullptr,
                 FILE_ATTRIBUTE_NORMAL,
                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                 FILE_OVERWRITE_IF, FILE_NON_DIRECTORY_FILE, nullptr, 0);
    if (!h) { printf("    SKIP: create failed\n"); return; }

    // Write via IO Ring first.
    {
      Ring wr;
      char data[] = "flush baseline data";
      auto *sqe =
          wr.push_write(h, data, sizeof(data) - 1, 0, 1,
                        IORING_SQE_FLAG_NONE);
      if (sqe)
        wr.submit_and_wait();
    }

    try_pipe_flush_mode(h, FLUSH_FLAGS_FILE_NORMAL, "disk DEFAULT");

    NtClose(h);

    // Cleanup.
    IO_STATUS_BLOCK diosb = {};
    HANDLE del = nullptr;
    test_support::init_object_attributes(
        &oa, &us, path_buf, test_support::wide_nul_terminated_length(path_buf));
    NtCreateFile(&del, DELETE_ACCESS | SYNCHRONIZE, &oa, &diosb, nullptr,
                 FILE_ATTRIBUTE_NORMAL, FILE_SHARE_DELETE, FILE_OPEN,
                 FILE_DELETE_ON_CLOSE | FILE_NON_DIRECTORY_FILE, nullptr, 0);
    if (del) NtClose(del);
  }

  // --- Part D: IO Ring flush on empty pipe (no prior write) ---
  {
    printf("\n  Part D: IO Ring flush on empty pipe\n");
    PipePair pp;
    if (!pp.ok) { printf("  SKIP\n"); return; }
    try_pipe_flush_mode(pp.wr, FLUSH_FLAGS_FILE_NORMAL, "empty DEFAULT");
  }
}

//===----------------------------------------------------------------------===//
// 5. Pipe: EOF when write end is closed
//===----------------------------------------------------------------------===//
static void test_pipe_eof() {
  printf("[TEST] Pipe: IO Ring read EOF after write-end close\n");

  PipePair pp;
  if (!pp.ok) { printf("  SKIP: pipe creation failed\n"); return; }

  // Close write end before reading.
  NtClose(pp.wr);
  pp.wr = nullptr;

  Ring r;
  char buf[64] = {};
  auto *sqe =
      r.push_read(pp.rd, buf, sizeof(buf), 0, 1, IORING_SQE_FLAG_NONE);
  CHECK(sqe != nullptr, "pipe EOF: queued SQE");
  if (!sqe)
    return;
  auto res = r.submit_and_wait();

  CHECK(res.got_cqe, "pipe EOF: got CQE");
  // STATUS_PIPE_BROKEN → HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE) = 0x8007006D
  // or bytes=0 with S_OK (like disk EOF)
  bool is_eof = (res.hr == S_OK && res.bytes == 0) ||
                res.hr == static_cast<HRESULT>(0x8007006DL) ||
                res.hr == static_cast<HRESULT>(0x80070026L);
  CHECK(is_eof, "pipe EOF: correct EOF/broken-pipe signal");
  printf("  EOF hr=0x%08X bytes=%d\n",
         static_cast<unsigned>(res.hr), static_cast<int>(res.bytes));
}

//===----------------------------------------------------------------------===//
// 6. Pipe: broken pipe when read end is closed (write should fail)
//===----------------------------------------------------------------------===//
static void test_pipe_broken() {
  printf("[TEST] Pipe: IO Ring write after read-end close (broken pipe)\n");

  PipePair pp;
  if (!pp.ok) { printf("  SKIP: pipe creation failed\n"); return; }

  // Close read end before writing.
  NtClose(pp.rd);
  pp.rd = nullptr;

  char data[] = "should fail";
  Ring r;
  auto *sqe =
      r.push_write(pp.wr, data, sizeof(data) - 1, 0, 1,
                   IORING_SQE_FLAG_NONE);
  CHECK(sqe != nullptr, "broken pipe: queued SQE");
  if (!sqe)
    return;
  auto res = r.submit_and_wait();

  CHECK(res.got_cqe, "broken pipe: got CQE");
  // Expect HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE) = 0x8007006D
  // or HRESULT_FROM_WIN32(ERROR_NO_DATA) = 0x800700E8
  bool is_broken = test_support::hresult_failed(res.hr);
  CHECK(is_broken, "broken pipe: write fails with error HRESULT");
  printf("  broken pipe hr=0x%08X bytes=%d\n",
         static_cast<unsigned>(res.hr), static_cast<int>(res.bytes));
}

//===----------------------------------------------------------------------===//
// 7. Pipe: IO Ring cancel (submit read, cancel before data arrives)
//===----------------------------------------------------------------------===//
static void test_pipe_cancel() {
  printf("[TEST] Pipe: IO Ring cancel pending read\n");

  PipePair pp;
  if (!pp.ok) { printf("  SKIP: pipe creation failed\n"); return; }

  // Submit a read with no data available — it will pend.
  Ring r;
  char buf[64] = {};
  ULONG_PTR tag = 42;
  auto *read_sqe = r.push_read(pp.rd, buf, sizeof(buf), 0, tag,
                               IORING_SQE_FLAG_NONE);
  CHECK(read_sqe != nullptr, "cancel: queued read SQE");
  if (!read_sqe)
    return;

  NTSTATUS submit_status = r.submit();
  CHECK(NT_SUCCESS(submit_status), "cancel: submitted read SQE");
  if (!NT_SUCCESS(submit_status))
    return;

  // Brief wait — read should NOT complete (no data).
  LARGE_INTEGER brief;
  brief.QuadPart = -50LL * 10000LL; // 50ms
  NTSTATUS ws = NtWaitForSingleObject(r.event, FALSE, &brief);

  // Check if CQE arrived (it shouldn't — pipe is empty).
  NT_IORING_CQE cqe = {};
  bool read_pended = !r.pop_cqe(&cqe);
  printf("  read pended (no data): %s\n", read_pended ? "yes" : "no");

  if (!read_pended) {
    // Read completed immediately — can't test cancel.
    printf("  SKIP: read completed immediately, can't test cancel\n");
    return;
  }

  // Cancel the pending read.
  auto *cancel_sqe = r.push_cancel(pp.rd, tag, tag + 1);
  CHECK(cancel_sqe != nullptr, "cancel: queued cancel SQE");
  if (!cancel_sqe)
    return;
  submit_status = r.submit();
  CHECK(NT_SUCCESS(submit_status), "cancel: submitted cancel SQE");
  if (!NT_SUCCESS(submit_status))
    return;

  // Wait for cancel + original CQEs.
  LARGE_INTEGER cancel_wait;
  cancel_wait.QuadPart = -500LL * 10000LL; // 500ms
  NtWaitForSingleObject(r.event, FALSE, &cancel_wait);

  // Drain all CQEs.
  int cqe_count = 0;
  HRESULT read_hr = static_cast<HRESULT>(0x80004005L);
  HRESULT cancel_hr = static_cast<HRESULT>(0x80004005L);
  for (int i = 0; i < 4; ++i) {
    NT_IORING_CQE c = {};
    if (r.pop_cqe(&c)) {
      ++cqe_count;
      if (c.UserData == tag)
        read_hr = c.ResultCode;
      else
        cancel_hr = c.ResultCode;
      printf("  CQE #%d: userData=%d hr=0x%08X bytes=%d\n",
             cqe_count, static_cast<int>(c.UserData),
             static_cast<unsigned>(c.ResultCode),
             static_cast<int>(c.Information));
    }
    if (cqe_count >= 2)
      break;
    // Brief wait for next CQE.
    LARGE_INTEGER bw;
    bw.QuadPart = -100LL * 10000LL;
    NtWaitForSingleObject(r.event, FALSE, &bw);
  }

  CHECK(cqe_count >= 1, "cancel: got at least 1 CQE");
  // The cancelled read should have a failure HRESULT (STATUS_CANCELLED).
  // 0xD0000120 = HRESULT_FROM_NT(STATUS_CANCELLED)
  bool read_cancelled = test_support::hresult_failed(read_hr) ||
                        read_hr == static_cast<HRESULT>(0xD0000120L);
  CHECK(read_cancelled, "cancel: read CQE shows cancelled/failed");
  printf("  >>> PIPE CANCEL %s\n",
         read_cancelled ? "WORKS" : "DOES NOT WORK");
}

//===----------------------------------------------------------------------===//
// 8. NUL device (FILE_TYPE_CHAR)
//===----------------------------------------------------------------------===//
static void test_nul_device() {
  printf("[TEST] NUL device (\\Device\\Null)\n");

  static constexpr WCHAR PATH[] = L"\\Device\\Null";
  UNICODE_STRING us;
  OBJECT_ATTRIBUTES oa;
  test_support::init_object_attributes(&oa, &us, PATH,
                                       sizeof(PATH) / sizeof(WCHAR) - 1);

  IO_STATUS_BLOCK iosb = {};
  HANDLE h = nullptr;
  NtOpenFile(&h, FILE_GENERIC_READ, &oa, &iosb,
             FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_NON_DIRECTORY_FILE);
  if (!h) { printf("  SKIP: NtOpenFile failed\n"); return; }

  Ring r;
  char buf[64] = {};
  auto *sqe = r.push_read(h, buf, sizeof(buf), 0, 1, IORING_SQE_FLAG_NONE);
  CHECK(sqe != nullptr, "NUL: queued SQE");
  if (!sqe) {
    NtClose(h);
    return;
  }
  auto res = r.submit_and_wait();

  CHECK(res.got_cqe, "NUL: got CQE");
  bool eof = (res.hr == S_OK && res.bytes == 0) ||
             res.hr == static_cast<HRESULT>(0x80070026L);
  CHECK(eof, "NUL: returns EOF (expected)");
  printf("  NUL hr=0x%08X bytes=%d\n",
         static_cast<unsigned>(res.hr), static_cast<int>(res.bytes));

  NtClose(h);
}

//===----------------------------------------------------------------------===//
// 9. IO Ring capabilities
//===----------------------------------------------------------------------===//
static void test_capabilities() {
  printf("[TEST] IO Ring capabilities\n");

  NT_IORING_CAPABILITIES caps = {};
  NTSTATUS status = ioring::query_capabilities(&caps);
  if (!NT_SUCCESS(status)) {
    printf("  SKIP: query_ioring_capabilities failed 0x%08X\n",
           static_cast<unsigned>(status));
    return;
  }

  printf("  MaxVersion:              %lu\n", caps.MaxVersion);
  printf("  MaxSubmissionQueueSize:  %lu\n", caps.MaxSubmissionQueueSize);
  printf("  MaxCompletionQueueSize:  %lu\n", caps.MaxCompletionQueueSize);
  printf("  Features:                0x%lX\n", caps.Features);

  bool emulated =
      (caps.Features & IORING_FEATURE_UM_EMULATION) != 0;
  printf("  Emulated:                %s\n", emulated ? "YES" : "NO");
}

int main() {
  printf("=== IO Ring Pipe Capability Probe ===\n\n");

  test_disk_read();
  printf("\n");
  test_pipe_read();
  printf("\n");
  test_pipe_write();
  printf("\n");
  test_pipe_flush();
  printf("\n");
  test_pipe_eof();
  printf("\n");
  test_pipe_broken();
  printf("\n");
  test_pipe_cancel();
  printf("\n");
  test_nul_device();
  printf("\n");
  test_capabilities();

  printf("\n=== Results: %d passed, %d failed ===\n",
         test_passes, test_failures);
  return test_failures ? 1 : 0;
}
