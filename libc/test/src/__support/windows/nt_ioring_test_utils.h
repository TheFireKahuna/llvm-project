#ifndef LLVM_LIBC_TEST_SRC___SUPPORT_WINDOWS_NT_IORING_TEST_UTILS_H
#define LLVM_LIBC_TEST_SRC___SUPPORT_WINDOWS_NT_IORING_TEST_UTILS_H

#include "src/__support/OSUtil/windows/nt/nt_ioring_ops.h"
#include "test/src/__support/windows/nt_test_utils.h"

namespace LIBC_NAMESPACE_DECL {
namespace test_support {

struct IoRing {
  ioring::RingState ring = {};
  HANDLE event = nullptr;
  bool ok = false;

  IoRing(ULONG sq_size = 8, ULONG cq_size = 16) {
    NTSTATUS status = ioring::create(&ring, IORING_VERSION_3, sq_size, cq_size);
    if (!NT_SUCCESS(status))
      return;

    status = NtCreateEvent(&event, EVENT_MODIFY_STATE | SYNCHRONIZE, nullptr,
                           SynchronizationEvent, FALSE);
    if (!NT_SUCCESS(status))
      return;

    status = ioring::set_completion_event(&ring, event);
    if (!NT_SUCCESS(status))
      return;

    ok = true;
  }

  ~IoRing() {
    if (event)
      NtClose(event);
    ioring::close(&ring);
  }

  struct Result {
    HRESULT hr = static_cast<HRESULT>(0x80004005L);
    ULONG_PTR bytes = 0;
    bool got_cqe = false;
    bool timed_out = false;
  };

  NT_IORING_SQE *push_read(HANDLE file, void *buffer, ULONG length,
                           ULONGLONG offset, ULONGLONG user_data,
                           ULONG flags = 0) {
    return ioring::push_read(&ring, file, buffer, length, offset, user_data,
                             flags);
  }

  NT_IORING_SQE *push_write(HANDLE file, const void *buffer, ULONG length,
                            ULONGLONG offset, ULONGLONG user_data,
                            ULONG flags = 0) {
    return ioring::push_write(&ring, file, buffer, length, offset, user_data,
                              flags);
  }

  NT_IORING_SQE *push_flush(HANDLE file, ULONGLONG user_data,
                            ULONG flush_mode = FLUSH_FLAGS_FILE_NORMAL,
                            ULONG flags = 0) {
    return ioring::push_flush(&ring, file, user_data, flush_mode, flags);
  }

  NT_IORING_SQE *push_cancel(HANDLE file, ULONGLONG cancel_id,
                             ULONGLONG user_data) {
    return ioring::push_cancel(&ring, file, cancel_id, user_data);
  }

  NTSTATUS submit(ULONG wait_ops = 0, LARGE_INTEGER *timeout = nullptr) {
    return ioring::submit(&ring, wait_ops, timeout);
  }

  bool pop_cqe(NT_IORING_CQE *out) { return ioring::pop_cqe(&ring, out); }

  Result submit_and_wait(int timeout_ms = 1000) {
    Result res = {};
    NTSTATUS status = submit();
    if (!NT_SUCCESS(status)) {
      res.hr = static_cast<HRESULT>(status);
      return res;
    }

    LARGE_INTEGER timeout;
    timeout.QuadPart = -static_cast<LONGLONG>(timeout_ms) * 10000LL;
    status = NtWaitForSingleObject(event, FALSE, &timeout);
    if (status == STATUS_TIMEOUT) {
      res.timed_out = true;
      return res;
    }
    if (!NT_SUCCESS(status)) {
      res.hr = static_cast<HRESULT>(status);
      return res;
    }

    NT_IORING_CQE cqe = {};
    if (pop_cqe(&cqe)) {
      res.got_cqe = true;
      res.hr = cqe.ResultCode;
      res.bytes = cqe.Information;
    }
    return res;
  }

  Result submit_and_wait_cancel_on_timeout(int timeout_ms = 1000,
                                           int drain_timeout_ms = 200,
                                           ULONGLONG cancel_user_data = 99) {
    Result res = {};
    NTSTATUS status = submit();
    if (!NT_SUCCESS(status)) {
      res.hr = static_cast<HRESULT>(status);
      return res;
    }

    NT_IORING_CQE cqe = {};
    if (pop_cqe(&cqe)) {
      res.got_cqe = true;
      res.hr = cqe.ResultCode;
      res.bytes = cqe.Information;
      return res;
    }

    LARGE_INTEGER timeout;
    timeout.QuadPart = -static_cast<LONGLONG>(timeout_ms) * 10000LL;
    status = NtWaitForSingleObject(event, FALSE, &timeout);
    if (status == STATUS_TIMEOUT) {
      res.timed_out = true;
      (void)push_cancel(nullptr, 0, cancel_user_data);
      (void)submit();

      LARGE_INTEGER drain_timeout;
      drain_timeout.QuadPart =
          -static_cast<LONGLONG>(drain_timeout_ms) * 10000LL;
      (void)NtWaitForSingleObject(event, FALSE, &drain_timeout);

      for (int i = 0; i < 4; ++i) {
        NT_IORING_CQE drain = {};
        if (!pop_cqe(&drain))
          break;
      }
      return res;
    }
    if (!NT_SUCCESS(status)) {
      res.hr = static_cast<HRESULT>(status);
      return res;
    }

    if (pop_cqe(&cqe)) {
      res.got_cqe = true;
      res.hr = cqe.ResultCode;
      res.bytes = cqe.Information;
    }
    return res;
  }
};

} // namespace test_support
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_TEST_SRC___SUPPORT_WINDOWS_NT_IORING_TEST_UTILS_H
