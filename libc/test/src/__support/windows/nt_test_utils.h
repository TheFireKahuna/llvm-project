#ifndef LLVM_LIBC_TEST_SRC___SUPPORT_WINDOWS_NT_TEST_UTILS_H
#define LLVM_LIBC_TEST_SRC___SUPPORT_WINDOWS_NT_TEST_UTILS_H

#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/nt/nt_path.h"
#include "src/__support/OSUtil/windows/io/file_type.h"
#include "src/__support/OSUtil/windows/temp_path.h"

namespace LIBC_NAMESPACE_DECL {
namespace test_support {

using ::FILE_TYPE_CHAR;
using ::FILE_TYPE_DISK;
using ::FILE_TYPE_PIPE;
using ::FILE_TYPE_UNKNOWN;
using ::S_FALSE;
using ::S_OK;
using ::THREAD_QUERY_INFORMATION;
using ::THREAD_QUERY_LIMITED_INFORMATION;
using ::THREAD_TERMINATE;
using ::init_object_attributes;
using windows::get_temp_path_w;
using windows::query_file_type;
// using windows::wide_nul_terminated_length;  // function removed; io_ring_*_test.cpp still references test_support::wide_nul_terminated_length and will need to be updated or re-ported.

inline constexpr DWORD INFINITE = 0xFFFFFFFF;
inline constexpr DWORD WAIT_OBJECT_0 = 0;
inline constexpr DWORD WAIT_TIMEOUT = 0x00000102;
inline constexpr DWORD WAIT_FAILED = 0xFFFFFFFF;

inline constexpr ACCESS_MASK TEST_THREAD_ACCESS =
    THREAD_TERMINATE | THREAD_QUERY_INFORMATION |
    THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;

// Thread-start routine invoked by the kernel under MS x64 ABI.
// `decltype` on an unreferenced external prototype — LIBC_MSABI attaches
// to the declaration, not to a type alias (trips `-Wgcc-compat`).
LIBC_MSABI DWORD __thread_start_type_source(void *);
using ThreadStartRoutine = decltype(&__thread_start_type_source);

inline HANDLE create_thread(ThreadStartRoutine start_routine, void *arg) {
  HANDLE thread = nullptr;
  NTSTATUS status = ::NtCreateThreadEx(
      &thread, TEST_THREAD_ACCESS, nullptr, NtCurrentProcess(),
      reinterpret_cast<PVOID>(start_routine), arg, 0, 0, 0, 0, nullptr);
  return NT_SUCCESS(status) ? thread : nullptr;
}

inline DWORD wait_for_single_object(HANDLE handle, DWORD milliseconds) {
  LARGE_INTEGER timeout = {};
  LARGE_INTEGER *timeout_ptr = nullptr;
  if (milliseconds != INFINITE) {
    timeout.QuadPart = -10000LL * static_cast<LONGLONG>(milliseconds);
    timeout_ptr = &timeout;
  }

  NTSTATUS status = ::NtWaitForSingleObject(handle, FALSE, timeout_ptr);
  if (status == STATUS_SUCCESS)
    return WAIT_OBJECT_0;
  if (status == STATUS_TIMEOUT)
    return WAIT_TIMEOUT;
  return WAIT_FAILED;
}

inline bool hresult_failed(HRESULT hr) { return hr < 0; }

inline bool is_valid_handle(HANDLE handle) {
  return handle && handle != INVALID_HANDLE_VALUE;
}

inline void sleep_ms(DWORD milliseconds) {
  LARGE_INTEGER interval = {};
  interval.QuadPart = -10000LL * static_cast<LONGLONG>(milliseconds);
  (void)::NtDelayExecution(FALSE, &interval);
}

// Alertable variant: wakes early when a user-mode APC is queued. Use when a
// test needs to drain the signal dispatcher's APC-based delivery path
// (cross-thread signals, SIGSTOP/SIGCONT, anything routed through
// NtQueueApcThreadEx2). A non-alertable wait will miss these entirely.
inline void alertable_sleep_ms(DWORD milliseconds) {
  LARGE_INTEGER interval = {};
  interval.QuadPart = -10000LL * static_cast<LONGLONG>(milliseconds);
  (void)::NtDelayExecution(TRUE, &interval);
}

inline bool write_handle(HANDLE handle, const void *buffer, DWORD length,
                         DWORD *bytes_written = nullptr) {
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtWriteFile(handle, nullptr, nullptr, nullptr, &iosb,
                                  const_cast<void *>(buffer), length, nullptr,
                                  nullptr);
  if (bytes_written)
    *bytes_written =
        NT_SUCCESS(status) ? static_cast<DWORD>(iosb.Information) : 0;
  return NT_SUCCESS(status);
}

} // namespace test_support
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_TEST_SRC___SUPPORT_WINDOWS_NT_TEST_UTILS_H
