//===-- Tree-scoped PTY namespace and ALPC join service -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/process/pty_tree.h"

#include "src/__support/CPP/scope_guard.h"
#include "src/__support/threads/windows/spin_wait.h"
#include "src/__support/OSUtil/windows/alloc/page_alloc.h"
#include "src/__support/OSUtil/windows/nt/nt_error.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/nt_ipc.h"
#include "src/__support/OSUtil/windows/nt/nt_job.h"
#include "src/__support/OSUtil/windows/nt/nt_memory_api.h"
#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/OSUtil/windows/nt/nt_security.h"
#include "src/__support/OSUtil/windows/nt/section_handle.h"
#include "src/__support/OSUtil/windows/nt/unicode_string_utils.h"
#include "src/__support/OSUtil/windows/process/console_handle_utils.h"
#include "src/__support/OSUtil/windows/process/nt_process_utils.h"
#include "src/__support/OSUtil/windows/process/spawn_runtime_data.h"
#include "src/__support/OSUtil/windows/security/security.h"
#include "src/__support/OSUtil/windows/signal/signal_internal.h"
#include "src/__support/OSUtil/windows/process/pty_reserved2.h"
#include "src/__support/OSUtil/windows/process/pty_shared_state.h"
#include "src/__support/OSUtil/windows/process/pty_tree_state.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/getsid.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/setpgid.h"
#include "src/__support/libc_assert.h"
#include "src/__support/threads/thread.h"
#include "src/__support/threads/raw_mutex.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace pty_tree {
namespace {

constexpr WCHAR NAMESPACE_PREFIX[] = u"NtPosixPtyTree-";
constexpr WCHAR SERVICE_PREFIX[] = u"PtyService-";
constexpr WCHAR ALLOC_SECTION_NAME[] = u"PtyAllocState";
constexpr WCHAR ALLOC_LOCK_NAME[] = u"PtyAllocLock";
constexpr WCHAR JOIN_LOCK_NAME[] = u"PtyJoinLock";
constexpr WCHAR TREE_JOB_NAME[] = u"TreeJob";

constexpr int FULL_OBJECT_NAME_CAP = 160;
constexpr ULONG ALPC_RECV_BUFFER_SIZE = 128;

struct PtyAllocState {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  cpp::Atomic<uint32_t> next_id;
};

constexpr uint32_t PTY_ALLOC_MAGIC = 0x434C4150; // 'PALC'
constexpr uint16_t PTY_ALLOC_VERSION = 1;

struct OwnedPtyEntry {
  uint32_t id;
  OwnedHandles handles;
  OwnedPtyEntry *next;
};

struct PtyJoinRequest {
  PORT_MESSAGE header;
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  uint32_t id;
};

struct PtyJoinReply {
  PORT_MESSAGE header;
  uint32_t magic;
  uint16_t version;
  uint16_t handle_count;
  NTSTATUS status;
  uint64_t reference;
  uint64_t state_lock;
  uint64_t state_section;
  uint64_t stdout_pipe;
};

struct ServiceThreadStartContext {
  HANDLE port;
  HANDLE ready_event;
};

constexpr uint32_t PTY_JOIN_MAGIC = 0x4A545950; // 'PYTJ'
constexpr uint16_t PTY_JOIN_VERSION = 1;
constexpr uint32_t PTY_JOIN_REPLY_MAGIC = 0x52545950; // 'PYTR'
constexpr uint16_t PTY_JOIN_REPLY_VERSION = 1;
constexpr ULONG PTY_JOIN_HANDLE_COUNT = 4;

// --- State-mutation ALPC messages (slave → master) ---
//
// Slaves hold read-only section mappings and cannot write shared state
// directly. Instead they send a PtyStateRequest to the master's ALPC
// service port. The master validates, applies the mutation under the
// kernel mutant (bumping change_seq), and replies with status.

constexpr uint32_t PTY_STATE_REQUEST_MAGIC = 0x53545950; // 'PYTS'
constexpr uint16_t PTY_STATE_REQUEST_VERSION = 1;
constexpr uint32_t PTY_STATE_REPLY_MAGIC = 0x52535950; // 'PYSR'
constexpr uint16_t PTY_STATE_REPLY_VERSION = 1;

// Operation codes for state mutation requests.
enum PtyStateOp : uint16_t {
  PTY_STATE_OP_SET_ATTR = 1,
  PTY_STATE_OP_SET_FOREGROUND_PGRP = 2,
  PTY_STATE_OP_SET_SESSION_CONTROLLER = 3,
};

struct PtySessionControllerUpdate {
  uint64_t controller_create_time;
  pid_t controlling_sid;
  pid_t foreground_pgrp;
  pid_t controller_pid;
  uint32_t reserved;
};

// Request: slave asks master to mutate shared state.
// Fits in 128-byte ALPC buffer: 24 (PORT_MESSAGE) + 12 (header) + 48 (termios) = 84 bytes max.
struct PtyStateRequest {
  PORT_MESSAGE header;
  uint32_t magic;
  uint16_t version;
  PtyStateOp op;
  uint32_t pty_id;
  union {
    struct termios attrs;                    // PTY_STATE_OP_SET_ATTR
    pid_t pgid;                              // PTY_STATE_OP_SET_FOREGROUND_PGRP
    PtySessionControllerUpdate controller;   // PTY_STATE_OP_SET_SESSION_CONTROLLER
  } payload;
};

// Reply: master reports success/failure.
struct PtyStateReply {
  PORT_MESSAGE header;
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  NTSTATUS status;
};

static_assert(sizeof(PtyStateRequest) <= ALPC_RECV_BUFFER_SIZE,
              "PtyStateRequest must fit in ALPC buffer");
static_assert(sizeof(PtyStateReply) <= ALPC_RECV_BUFFER_SIZE,
              "PtyStateReply must fit in ALPC buffer");

void generate_tree_nonce(uint8_t (&nonce)[16]);

static RawMutex owned_pty_lock;
static OwnedPtyEntry *owned_pty_head = nullptr;
static PtyTreeState pty_tree_state = {};

PtyTreeState &state() { return pty_tree_state; }

bool has_nonce_bytes(const uint8_t *nonce) {
  for (size_t i = 0; i < 16; ++i) {
    if (nonce[i] != 0)
      return true;
  }
  return false;
}

DWORD current_pid() {
  return static_cast<DWORD>(static_cast<uintptr_t>(NtCurrentProcessId()));
}

SID *query_token_user_sid(HANDLE token, void *buffer, ULONG size) {
  ULONG needed = 0;
  NTSTATUS status =
      ::NtQueryInformationToken(token, TokenUser, buffer, size, &needed);
  if (!NT_SUCCESS(status))
    return nullptr;
  return reinterpret_cast<TOKEN_USER *>(buffer)->User.Sid;
}

size_t append_decimal(WCHAR *buffer, size_t pos, size_t cap, uint32_t value) {
  WCHAR tmp[16];
  size_t len = 0;
  if (value == 0) {
    tmp[len++] = u'0';
  } else {
    while (value != 0 && len < 16) {
      tmp[len++] = static_cast<WCHAR>(u'0' + (value % 10));
      value /= 10;
    }
  }
  for (size_t i = 0; i < len && pos + 1 < cap; ++i)
    buffer[pos++] = tmp[len - i - 1];
  return pos;
}

size_t append_hex64(WCHAR *buffer, size_t pos, size_t cap, uint64_t value) {
  static constexpr WCHAR HEX[] = u"0123456789ABCDEF";
  for (int i = 15; i >= 0 && pos + 1 < cap; --i)
    buffer[pos++] = HEX[(value >> (i * 4)) & 0x0F];
  return pos;
}

size_t append_namespace_name(WCHAR *buffer, size_t pos, size_t cap) {
  if (pos >= cap)
    return pos;

  static constexpr WCHAR HEX[] = u"0123456789ABCDEF";
  for (size_t i = 0;
       i < sizeof(NAMESPACE_PREFIX) / sizeof(WCHAR) - 1 && pos + 1 < cap; ++i)
    buffer[pos++] = NAMESPACE_PREFIX[i];

  for (size_t i = 0; i < 16 && pos + 2 < cap; ++i) {
    buffer[pos++] = HEX[state().tree_nonce[i] >> 4];
    buffer[pos++] = HEX[state().tree_nonce[i] & 0x0F];
  }
  return pos;
}

size_t format_namespace_name(WCHAR *buffer, size_t cap) {
  size_t pos = 0;
  pos = append_namespace_name(buffer, pos, cap);
  buffer[pos] = 0;
  return pos;
}

[[maybe_unused]] size_t format_leaf_name(WCHAR *buffer, size_t cap,
                                         const WCHAR *leaf) {
  size_t pos = 0;
  for (size_t i = 0; leaf[i] != 0 && pos + 1 < cap; ++i)
    buffer[pos++] = leaf[i];
  buffer[pos] = 0;
  return pos;
}

[[maybe_unused]] size_t format_id_leaf_name(WCHAR *buffer, size_t cap,
                                            const WCHAR *prefix,
                                            uint32_t id) {
  size_t pos = 0;
  for (size_t i = 0; prefix[i] != 0 && pos + 1 < cap; ++i)
    buffer[pos++] = prefix[i];
  pos = append_decimal(buffer, pos, cap, id);
  buffer[pos] = 0;
  return pos;
}

void init_named_attributes(OBJECT_ATTRIBUTES *oa, UNICODE_STRING *name,
                           HANDLE root, WCHAR *buffer, size_t length) {
  windows_util::init_unicode_string(name, buffer, length);
  oa->Length = sizeof(OBJECT_ATTRIBUTES);
  oa->RootDirectory = root;
  oa->ObjectName = name;
  oa->Attributes = OBJ_CASE_INSENSITIVE;
  oa->SecurityDescriptor = nullptr;
  oa->SecurityQualityOfService = nullptr;
}

POBJECT_BOUNDARY_DESCRIPTOR create_boundary_descriptor_locked() {
  WCHAR ns_buf[FULL_OBJECT_NAME_CAP] = {};
  size_t ns_len = format_namespace_name(ns_buf, FULL_OBJECT_NAME_CAP);
  (void)ns_len;
  LIBC_ASSERT(ns_len + 1 <= FULL_OBJECT_NAME_CAP &&
              "format namespace name truncated");
  UNICODE_STRING ns_name = {};
  windows_util::init_unicode_string(&ns_name, ns_buf, ns_len);

  POBJECT_BOUNDARY_DESCRIPTOR boundary =
      ::RtlCreateBoundaryDescriptor(&ns_name, BOUNDARY_DESCRIPTOR_FLAG_NONE);
  if (!boundary)
    return nullptr;

  alignas(8) UCHAR token_buf[256];
  SID *user_sid =
      query_token_user_sid(NtCurrentProcessToken(), token_buf, sizeof(token_buf));
  if (!user_sid) {
    ::RtlDeleteBoundaryDescriptor(boundary);
    return nullptr;
  }

  NTSTATUS status = ::RtlAddSIDToBoundaryDescriptor(&boundary, user_sid);
  if (!NT_SUCCESS(status)) {
    ::RtlDeleteBoundaryDescriptor(boundary);
    return nullptr;
  }

  SID integrity_sid = {};
  SID_IDENTIFIER_AUTHORITY label_auth = {
      SECURITY_MANDATORY_LABEL_AUTHORITY_VALUE[0],
      SECURITY_MANDATORY_LABEL_AUTHORITY_VALUE[1],
      SECURITY_MANDATORY_LABEL_AUTHORITY_VALUE[2],
      SECURITY_MANDATORY_LABEL_AUTHORITY_VALUE[3],
      SECURITY_MANDATORY_LABEL_AUTHORITY_VALUE[4],
      SECURITY_MANDATORY_LABEL_AUTHORITY_VALUE[5]};
  ::RtlInitializeSid(&integrity_sid, &label_auth, 1);
  *::RtlSubAuthoritySid(&integrity_sid, 0) = SECURITY_MANDATORY_MEDIUM_RID;

  status =
      ::RtlAddIntegrityLabelToBoundaryDescriptor(&boundary, &integrity_sid);
  if (!NT_SUCCESS(status)) {
    ::RtlDeleteBoundaryDescriptor(boundary);
    return nullptr;
  }

  return boundary;
}

NTSTATUS ensure_namespace_locked() {
  if (state().namespace_handle)
    return STATUS_SUCCESS;

  if (!state().boundary_descriptor) {
    state().boundary_descriptor = create_boundary_descriptor_locked();
    if (!state().boundary_descriptor)
      return STATUS_UNSUCCESSFUL;
  }

  WCHAR ns_buf[FULL_OBJECT_NAME_CAP] = {};
  size_t ns_len = format_namespace_name(ns_buf, FULL_OBJECT_NAME_CAP);
  (void)ns_len;
  LIBC_ASSERT(ns_len + 1 <= FULL_OBJECT_NAME_CAP &&
              "format namespace name truncated");
  UNICODE_STRING ns_name = {};
  windows_util::init_unicode_string(&ns_name, ns_buf, ns_len);
  OBJECT_ATTRIBUTES oa = windows::named_internal_oa(&ns_name);

  auto *boundary = reinterpret_cast<POBJECT_BOUNDARY_DESCRIPTOR>(
      state().boundary_descriptor);
  HANDLE ns = nullptr;
  NTSTATUS status =
      ::NtCreatePrivateNamespace(&ns, MAXIMUM_ALLOWED, &oa, boundary);
  if (status == STATUS_OBJECT_NAME_COLLISION)
    status = ::NtOpenPrivateNamespace(&ns, MAXIMUM_ALLOWED, &oa, boundary);
  if (!NT_SUCCESS(status))
    return status;

  state().namespace_handle = ns;
  return STATUS_SUCCESS;
}

NTSTATUS map_section_rw(HANDLE section, SIZE_T size, void **view) {
  if (!view)
    return STATUS_INVALID_PARAMETER;
  *view = nullptr;
  LARGE_INTEGER offset = {};
  SIZE_T view_size = size;
  PVOID base = nullptr;
  NTSTATUS status = ::NtMapViewOfSectionEx(section, NtCurrentProcess(), &base,
                                           &offset, &view_size, 0,
                                           PAGE_READWRITE, nullptr, 0);
  if (!NT_SUCCESS(status))
    return status;
  *view = base;
  return STATUS_SUCCESS;
}

NTSTATUS map_section_ro(HANDLE section, SIZE_T size, void **view) {
  if (!view)
    return STATUS_INVALID_PARAMETER;
  *view = nullptr;
  LARGE_INTEGER offset = {};
  SIZE_T view_size = size;
  PVOID base = nullptr;
  NTSTATUS status = ::NtMapViewOfSectionEx(section, NtCurrentProcess(), &base,
                                           &offset, &view_size, 0,
                                           PAGE_READONLY, nullptr, 0);
  if (!NT_SUCCESS(status))
    return status;
  *view = base;
  return STATUS_SUCCESS;
}

void unmap_view_if_needed(void **view) {
  if (view && *view) {
    ::NtUnmapViewOfSectionEx(NtCurrentProcess(), *view, 0);
    *view = nullptr;
  }
}

NTSTATUS create_named_section_locked(const WCHAR *leaf, SIZE_T size,
                                     HANDLE *section_out) {
  if (!leaf || !section_out)
    return STATUS_INVALID_PARAMETER;

  *section_out = nullptr;

  WCHAR name_buf[FULL_OBJECT_NAME_CAP] = {};
  size_t name_len = format_leaf_name(name_buf, FULL_OBJECT_NAME_CAP, leaf);
  (void)name_len;
  LIBC_ASSERT(name_len + 1 <= FULL_OBJECT_NAME_CAP &&
              "format object name truncated");

  UNICODE_STRING name = {};
  OBJECT_ATTRIBUTES oa = {};
  init_named_attributes(&oa, &name, state().namespace_handle, name_buf,
                        name_len);

  LARGE_INTEGER section_size = {};
  section_size.QuadPart = static_cast<LONGLONG>(size);
  HANDLE section = nullptr;
  NTSTATUS status = ::NtCreateSectionEx(
      &section, SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_QUERY, &oa,
      &section_size, PAGE_READWRITE, SEC_COMMIT, nullptr, nullptr, 0);
  if (!NT_SUCCESS(status))
    return status;

  *section_out = section;
  return STATUS_SUCCESS;
}

NTSTATUS create_named_mutant_locked(const WCHAR *leaf, HANDLE *mutant_out) {
  if (!leaf || !mutant_out)
    return STATUS_INVALID_PARAMETER;

  *mutant_out = nullptr;

  WCHAR name_buf[FULL_OBJECT_NAME_CAP] = {};
  size_t name_len = format_leaf_name(name_buf, FULL_OBJECT_NAME_CAP, leaf);
  (void)name_len;
  LIBC_ASSERT(name_len + 1 <= FULL_OBJECT_NAME_CAP &&
              "format object name truncated");

  UNICODE_STRING name = {};
  OBJECT_ATTRIBUTES oa = {};
  init_named_attributes(&oa, &name, state().namespace_handle, name_buf,
                        name_len);

  HANDLE mutant = nullptr;
  NTSTATUS status =
      ::NtCreateMutant(&mutant, MUTANT_ALL_ACCESS, &oa, FALSE);
  if (NT_SUCCESS(status) || status == STATUS_OBJECT_NAME_COLLISION) {
    if (!mutant)
      return STATUS_UNSUCCESSFUL;
    *mutant_out = mutant;
    return STATUS_SUCCESS;
  }

  return status;
}

int create_shared_state_locked(uint32_t id, HANDLE *state_lock,
                               HANDLE *state_section,
                               PtySharedState **state_view) {
  auto mutant_oa = windows::internal_oa();
  NTSTATUS status =
      ::NtCreateMutant(state_lock, MUTANT_ALL_ACCESS, &mutant_oa, FALSE);
  if (!NT_SUCCESS(status))
    return windows_util::ntstatus_to_errno(status);

  windows::SectionHandle section =
      windows::SectionHandle::create_anon_rw(sizeof(PtySharedState), &status);
  if (!section) {
    console_util::close_handle_if_valid(state_lock);
    return windows_util::ntstatus_to_errno(status);
  }

  HANDLE raw_section = section.release();
  status = map_section_rw(raw_section, sizeof(PtySharedState),
                          reinterpret_cast<void **>(state_view));
  if (!NT_SUCCESS(status)) {
    console_util::close_handle_if_valid(state_lock);
    console_util::close_handle_if_valid(&raw_section);
    return windows_util::ntstatus_to_errno(status);
  }

  *state_section = raw_section;
  __builtin_memset(*state_view, 0, sizeof(PtySharedState));
  (*state_view)->magic = PTY_SHARED_STATE_MAGIC;
  (*state_view)->version = PTY_SHARED_STATE_VERSION;
  (*state_view)->id = id;
  return 0;
}

int validate_shared_state_view(HANDLE state_section, uint32_t expected_id,
                               PtySharedState **state_view) {
  if (!state_section || !state_view)
    return EINVAL;

  *state_view = nullptr;
  // Try read-only first (slaves have SECTION_MAP_READ only handles).
  // Fall back to read-write for master processes that hold full access.
  NTSTATUS status = map_section_ro(state_section, sizeof(PtySharedState),
                                   reinterpret_cast<void **>(state_view));
  if (!NT_SUCCESS(status)) {
    status = map_section_rw(state_section, sizeof(PtySharedState),
                            reinterpret_cast<void **>(state_view));
  }
  if (!NT_SUCCESS(status))
    return windows_util::ntstatus_to_errno(status);

  if ((*state_view)->magic == PTY_SHARED_STATE_MAGIC &&
      ((*state_view)->version == PTY_SHARED_STATE_VERSION ||
       (*state_view)->version == 2) &&
      (expected_id == 0 || (*state_view)->id == expected_id)) {
    return 0;
  }

  void *view = *state_view;
  unmap_view_if_needed(&view);
  *state_view = nullptr;
  return EIO;
}

NTSTATUS ensure_tree_job_locked() {
  if (state().tree_job) {
    NTSTATUS membership =
        ::NtIsProcessInJob(NtCurrentProcess(), state().tree_job);
    if (membership == STATUS_PROCESS_IN_JOB)
      return STATUS_SUCCESS;
    console_util::close_handle_if_valid(&state().tree_job);
  }

  NTSTATUS status = ensure_namespace_locked();
  if (!NT_SUCCESS(status))
    return status;

  WCHAR name_buf[FULL_OBJECT_NAME_CAP] = {};
  size_t name_len = format_leaf_name(name_buf, FULL_OBJECT_NAME_CAP,
                                     TREE_JOB_NAME);
  (void)name_len;
  LIBC_ASSERT(name_len + 1 <= FULL_OBJECT_NAME_CAP &&
              "format leaf name truncated");

  UNICODE_STRING name = {};
  OBJECT_ATTRIBUTES oa = {};
  init_named_attributes(&oa, &name, state().namespace_handle, name_buf,
                        name_len);

  HANDLE job = nullptr;
  status = ::NtOpenJobObject(&job, JOB_OBJECT_ALL_ACCESS, &oa);
  if (status == STATUS_OBJECT_NAME_NOT_FOUND)
    status = ::NtCreateJobObject(&job, JOB_OBJECT_ALL_ACCESS, &oa);
  if (!NT_SUCCESS(status))
    return status;

  NTSTATUS membership = ::NtIsProcessInJob(NtCurrentProcess(), job);
  if (membership != STATUS_PROCESS_IN_JOB) {
    status = ::NtAssignProcessToJobObject(job, NtCurrentProcess());
    if (!NT_SUCCESS(status)) {
      ::NtClose(job);
      return status;
    }
  }

  state().tree_job = job;
  return STATUS_SUCCESS;
}

NTSTATUS wait_mutant(HANDLE mutant) {
  NTSTATUS status = ::NtWaitForSingleObject(mutant, FALSE, nullptr);
  if (status == STATUS_ABANDONED)
    return STATUS_SUCCESS;
  return status;
}

void release_mutant(HANDLE mutant) {
  if (mutant)
    (void)::NtReleaseMutant(mutant, nullptr);
}

// --- Seqlock helpers for PtySharedState ---
//
// Writers (master process, under mutant):
//   seqlock_write_begin(shared)  → increments change_seq to odd (write-in-progress)
//   seqlock_write_end(shared)    → increments change_seq to even (consistent)
//
// Readers (slave processes, PAGE_READONLY mapping, no mutant):
//   seq = seqlock_read_begin(shared)  → load change_seq; retry if odd
//   ... read fields ...
//   seqlock_read_retry(shared, seq)   → true if change_seq differs (must retry)

void seqlock_write_begin(PtySharedState *shared) {
  uint32_t seq = __atomic_load_n(&shared->change_seq, __ATOMIC_RELAXED);
  __atomic_store_n(&shared->change_seq, seq + 1, __ATOMIC_RELEASE);
}

void seqlock_write_end(PtySharedState *shared) {
  __atomic_thread_fence(__ATOMIC_RELEASE);
  uint32_t seq = __atomic_load_n(&shared->change_seq, __ATOMIC_RELAXED);
  __atomic_store_n(&shared->change_seq, seq + 1, __ATOMIC_RELEASE);
}

uint32_t seqlock_read_begin(const PtySharedState *shared) {
  uint32_t seq;
  for (;;) {
    seq = __atomic_load_n(&shared->change_seq, __ATOMIC_ACQUIRE);
    if ((seq & 1) == 0)
      return seq;
    // Writer in progress — spin briefly.
    spin_wait::relax_processor();
  }
}

bool seqlock_read_retry(const PtySharedState *shared,
                                    uint32_t start_seq) {
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  return __atomic_load_n(&shared->change_seq, __ATOMIC_RELAXED) != start_seq;
}

NTSTATUS alloc_state_view_locked(HANDLE *section_out, PtyAllocState **view_out) {
  if (!section_out || !view_out)
    return STATUS_INVALID_PARAMETER;

  if (state().alloc_section && state().alloc_state_view) {
    *section_out = state().alloc_section;
    *view_out = static_cast<PtyAllocState *>(state().alloc_state_view);
    return STATUS_SUCCESS;
  }

  *section_out = nullptr;
  *view_out = nullptr;

  NTSTATUS status = ensure_namespace_locked();
  if (!NT_SUCCESS(status))
    return status;

  HANDLE raw_section = nullptr;
  status = create_named_section_locked(ALLOC_SECTION_NAME,
                                       sizeof(PtyAllocState), &raw_section);
  if (!NT_SUCCESS(status))
    return status;
  windows::SectionHandle section = windows::SectionHandle::adopt(raw_section);

  void *view = nullptr;
  status = map_section_rw(section.get(), sizeof(PtyAllocState), &view);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  auto *alloc_state = static_cast<PtyAllocState *>(view);
  if (alloc_state->magic != PTY_ALLOC_MAGIC ||
      alloc_state->version != PTY_ALLOC_VERSION) {
    alloc_state->magic = PTY_ALLOC_MAGIC;
    alloc_state->version = PTY_ALLOC_VERSION;
    alloc_state->reserved = 0;
    alloc_state->next_id.store(1, cpp::MemoryOrder::RELAXED);
  }
  state().alloc_section = section.release();
  state().alloc_state_view = alloc_state;
  *section_out = state().alloc_section;
  *view_out = static_cast<PtyAllocState *>(state().alloc_state_view);
  return STATUS_SUCCESS;
}

NTSTATUS open_alloc_lock_locked(HANDLE *lock_out) {
  if (!lock_out)
    return STATUS_INVALID_PARAMETER;

  if (state().alloc_lock) {
    *lock_out = state().alloc_lock;
    return STATUS_SUCCESS;
  }

  *lock_out = nullptr;

  NTSTATUS status = ensure_namespace_locked();
  if (!NT_SUCCESS(status))
    return status;

  HANDLE lock = nullptr;
  status = create_named_mutant_locked(ALLOC_LOCK_NAME, &lock);
  if (!NT_SUCCESS(status))
    return status;
  state().alloc_lock = lock;
  *lock_out = state().alloc_lock;
  return STATUS_SUCCESS;
}

NTSTATUS open_join_lock_locked(HANDLE *lock_out) {
  if (!lock_out)
    return STATUS_INVALID_PARAMETER;

  *lock_out = nullptr;

  NTSTATUS status = ensure_namespace_locked();
  if (!NT_SUCCESS(status))
    return status;

  HANDLE lock = nullptr;
  status = create_named_mutant_locked(JOIN_LOCK_NAME, &lock);
  if (!NT_SUCCESS(status))
    return status;
  *lock_out = lock;
  return STATUS_SUCCESS;
}

size_t format_service_leaf_name(WCHAR *buffer, size_t cap, DWORD pid,
                                uint64_t create_time) {
  size_t pos = 0;
  for (size_t i = 0; i < sizeof(SERVICE_PREFIX) / sizeof(WCHAR) - 1 &&
                     pos + 1 < cap;
       ++i)
    buffer[pos++] = SERVICE_PREFIX[i];
  pos = append_decimal(buffer, pos, cap, pid);
  if (pos + 1 < cap)
    buffer[pos++] = u'-';
  pos = append_hex64(buffer, pos, cap, create_time);
  buffer[pos] = 0;
  return pos;
}

[[maybe_unused]] ACL *build_service_port_dacl() {
  alignas(8) UCHAR token_buf[256];
  SID *user_sid =
      query_token_user_sid(NtCurrentProcessToken(), token_buf, sizeof(token_buf));
  if (!user_sid)
    return nullptr;

  ULONG user_sid_len = ::RtlLengthSid(user_sid);

  SID admin_sid = {};
  SID_IDENTIFIER_AUTHORITY nt_auth = {SECURITY_NT_AUTHORITY_VALUE[0],
                                      SECURITY_NT_AUTHORITY_VALUE[1],
                                      SECURITY_NT_AUTHORITY_VALUE[2],
                                      SECURITY_NT_AUTHORITY_VALUE[3],
                                      SECURITY_NT_AUTHORITY_VALUE[4],
                                      SECURITY_NT_AUTHORITY_VALUE[5]};
  ::RtlInitializeSid(&admin_sid, &nt_auth, 2);
  *::RtlSubAuthoritySid(&admin_sid, 0) = SECURITY_BUILTIN_DOMAIN_RID;
  *::RtlSubAuthoritySid(&admin_sid, 1) = DOMAIN_ALIAS_RID_ADMINS;
  ULONG admin_sid_len = ::RtlLengthSid(&admin_sid);

  ULONG ace1_size = sizeof(ACE_HEADER) + sizeof(ACCESS_MASK) + user_sid_len;
  ULONG ace2_size = sizeof(ACE_HEADER) + sizeof(ACCESS_MASK) + admin_sid_len;
  ULONG acl_size = sizeof(ACL) + ace1_size + ace2_size;

  ACL *dacl = static_cast<ACL *>(page_alloc(acl_size));
  if (!dacl)
    return nullptr;
  dacl->AclRevision = ACL_REVISION;
  dacl->Sbz1 = 0;
  dacl->AclSize = static_cast<USHORT>(acl_size);
  dacl->AceCount = 2;
  dacl->Sbz2 = 0;

  auto *ace1 = reinterpret_cast<ACCESS_ALLOWED_ACE *>(
      reinterpret_cast<UCHAR *>(dacl) + sizeof(ACL));
  ace1->Header.AceType = ACCESS_ALLOWED_ACE_TYPE;
  ace1->Header.AceFlags = 0;
  ace1->Header.AceSize = static_cast<USHORT>(ace1_size);
  ace1->Mask = 0x10000000;
  ::RtlCopySid(user_sid_len, reinterpret_cast<SID *>(&ace1->SidStart),
               user_sid);

  auto *ace2 = reinterpret_cast<ACCESS_ALLOWED_ACE *>(
      reinterpret_cast<UCHAR *>(ace1) + ace1_size);
  ace2->Header.AceType = ACCESS_ALLOWED_ACE_TYPE;
  ace2->Header.AceFlags = 0;
  ace2->Header.AceSize = static_cast<USHORT>(ace2_size);
  ace2->Mask = 0x10000000;
  ::RtlCopySid(admin_sid_len, reinterpret_cast<SID *>(&ace2->SidStart),
               &admin_sid);
  return dacl;
}

[[maybe_unused]] void build_service_port_sd(SECURITY_DESCRIPTOR *sd, ACL *dacl) {
  sd->Revision = SECURITY_DESCRIPTOR_REVISION;
  sd->Sbz1 = 0;
  sd->Control = SE_DACL_PRESENT | SE_DACL_PROTECTED;
  sd->Owner = nullptr;
  sd->Group = nullptr;
  sd->Sacl = nullptr;
  sd->Dacl = dacl;
}

OwnedPtyEntry *find_owned_locked(uint32_t id) {
  for (OwnedPtyEntry *entry = owned_pty_head; entry; entry = entry->next) {
    if (entry->id == id)
      return entry;
  }
  return nullptr;
}

HANDLE duplicate_handle_for_pid(HANDLE handle, DWORD pid,
                                ACCESS_MASK desired_access = 0,
                                bool restrict_access = false) {
  if (!handle)
    return nullptr;
  constexpr ACCESS_MASK PROCESS_DUP_HANDLE = 0x0040;
  HANDLE target = nullptr;
  NTSTATUS status = ::NtOpenProcessById(
      &target, PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION, pid);
  if (!NT_SUCCESS(status)) {
    return nullptr;
  }

  HANDLE remote = nullptr;
  ULONG options = restrict_access ? 0u : DUPLICATE_SAME_ACCESS;
  ACCESS_MASK access = restrict_access ? desired_access : 0;
  status = ::NtDuplicateObject(NtCurrentProcess(), handle, target, &remote,
                               access, 0, options);
  ::NtClose(target);
  if (!NT_SUCCESS(status)) {
    return nullptr;
  }
  return remote;
}

void close_owned_handles(OwnedHandles &handles) {
  if (handles.state_section)
    ::NtClose(handles.state_section);
  if (handles.state_lock)
    ::NtClose(handles.state_lock);
  if (handles.stdout_pipe)
    ::NtClose(handles.stdout_pipe);
  if (handles.error)
    ::NtClose(handles.error);
  if (handles.output)
    ::NtClose(handles.output);
  if (handles.input)
    ::NtClose(handles.input);
  if (handles.connection)
    ::NtClose(handles.connection);
  if (handles.reference)
    ::NtClose(handles.reference);
  handles = {};
}

OwnedHandles duplicate_owned_handles_for_pid(const OwnedHandles &source,
                                             DWORD pid) {
  OwnedHandles result = {};
  result.state_lock = duplicate_handle_for_pid(source.state_lock, pid);
  if (!result.state_lock)
    return {};
  result.state_section = duplicate_handle_for_pid(source.state_section, pid);
  if (!result.state_section) {
    close_owned_handles(result);
    return {};
  }
  result.reference = duplicate_handle_for_pid(source.reference, pid);
  if (!result.reference)
    goto fail;
  result.connection = duplicate_handle_for_pid(source.connection, pid);
  if (!result.connection) {
    goto fail;
  }
  result.input = duplicate_handle_for_pid(source.input, pid);
  if (!result.input) {
    goto fail;
  }
  result.output = duplicate_handle_for_pid(source.output, pid);
  if (!result.output) {
    goto fail;
  }
  result.error = duplicate_handle_for_pid(source.error, pid);
  if (!result.error) {
    goto fail;
  }
  result.stdout_pipe = duplicate_handle_for_pid(source.stdout_pipe, pid);
  if (!result.stdout_pipe) {
    goto fail;
  }
  return result;

fail:
  close_owned_handles(result);
  return {};
}

OwnedHandles duplicate_join_handles_for_pid(const OwnedHandles &source,
                                            DWORD pid) {
  OwnedHandles result = {};
  result.state_lock = duplicate_handle_for_pid(source.state_lock, pid);
  if (!result.state_lock)
    return {};
  // Slaves get SECTION_MAP_READ | SECTION_QUERY only — they cannot write to
  // shared state directly. Mutations go through ALPC to the master.
  constexpr ACCESS_MASK SECTION_READ_ONLY =
      SECTION_MAP_READ | SECTION_QUERY;
  result.state_section = duplicate_handle_for_pid(
      source.state_section, pid, SECTION_READ_ONLY, true);
  if (!result.state_section) {
    close_owned_handles(result);
    return {};
  }
  result.reference = duplicate_handle_for_pid(source.reference, pid);
  if (!result.reference) {
    close_owned_handles(result);
    return {};
  }
  result.stdout_pipe = duplicate_handle_for_pid(source.stdout_pipe, pid);
  if (!result.stdout_pipe) {
    close_owned_handles(result);
    return {};
  }
  return result;
}

OwnedHandles accept_join_request(uint32_t id) {
  owned_pty_lock.lock();
  OwnedPtyEntry *entry = find_owned_locked(id);
  OwnedHandles handles = entry ? entry->handles : OwnedHandles{};
  owned_pty_lock.unlock();
  if (!handles.state_lock || !handles.state_section || !handles.reference ||
      !handles.connection || !handles.input ||
      !handles.output || !handles.error || !handles.stdout_pipe) {
    return {};
  }
  return handles;
}

void *service_thread_main(void *context);

// Handle a state-mutation request from a slave process (ALPC connection).
// The master applies the mutation under the kernel mutant with seqlock.
void handle_state_request(HANDLE server_port, PPORT_MESSAGE request) {
  if (!request)
    return;

  auto *state_req = reinterpret_cast<const PtyStateRequest *>(request);
  bool valid =
      static_cast<unsigned short>(request->u1.s1.TotalLength) >=
          sizeof(PtyStateRequest) &&
      state_req->magic == PTY_STATE_REQUEST_MAGIC &&
      state_req->version == PTY_STATE_REQUEST_VERSION;

  PtyStateReply reply = {};
  reply.header.u1.s1.DataLength =
      static_cast<CSHORT>(sizeof(PtyStateReply) - sizeof(PORT_MESSAGE));
  reply.header.u1.s1.TotalLength = static_cast<CSHORT>(sizeof(PtyStateReply));
  reply.header.MessageId = request->MessageId;
  reply.magic = PTY_STATE_REPLY_MAGIC;
  reply.version = PTY_STATE_REPLY_VERSION;
  reply.reserved = 0;
  reply.status = STATUS_INVALID_PARAMETER;

  if (valid) {
    owned_pty_lock.lock();
    OwnedPtyEntry *entry = find_owned_locked(state_req->pty_id);
    HANDLE mutant = entry ? entry->handles.state_lock : nullptr;
    HANDLE section = entry ? entry->handles.state_section : nullptr;
    owned_pty_lock.unlock();

    if (mutant && section) {
      PtySharedState *view = nullptr;
      NTSTATUS map_status = map_section_rw(section, sizeof(PtySharedState),
                                           reinterpret_cast<void **>(&view));
      if (NT_SUCCESS(map_status) && view) {
        NTSTATUS lock_status = wait_mutant(mutant);
        if (NT_SUCCESS(lock_status)) {
          seqlock_write_begin(view);
          switch (state_req->op) {
          case PTY_STATE_OP_SET_ATTR:
            view->attrs = state_req->payload.attrs;
            reply.status = STATUS_SUCCESS;
            break;
          case PTY_STATE_OP_SET_FOREGROUND_PGRP:
            if (state_req->payload.pgid > 0) {
              view->foreground_pgrp = state_req->payload.pgid;
              reply.status = STATUS_SUCCESS;
            }
            break;
          case PTY_STATE_OP_SET_SESSION_CONTROLLER:
            if (state_req->payload.controller.controlling_sid > 0 &&
                state_req->payload.controller.foreground_pgrp > 0 &&
                state_req->payload.controller.controller_pid > 0) {
              view->controlling_sid =
                  state_req->payload.controller.controlling_sid;
              view->foreground_pgrp =
                  state_req->payload.controller.foreground_pgrp;
              view->controller_pid =
                  state_req->payload.controller.controller_pid;
              view->controller_create_time =
                  state_req->payload.controller.controller_create_time;
              reply.status = STATUS_SUCCESS;
            }
            break;
          }
          seqlock_write_end(view);
          release_mutant(mutant);
        } else {
          reply.status = lock_status;
        }
        void *raw_view = view;
        unmap_view_if_needed(&raw_view);
      } else {
        reply.status = map_status;
      }
    } else {
      reply.status = STATUS_OBJECT_NAME_NOT_FOUND;
    }
  }

  HANDLE accepted = nullptr;
  ALPC_PORT_ATTRIBUTES attrs = {};
  attrs.SecurityQos.Length = sizeof(SECURITY_QUALITY_OF_SERVICE);
  attrs.SecurityQos.ImpersonationLevel = SecurityIdentification;
  attrs.SecurityQos.ContextTrackingMode = 0;
  attrs.SecurityQos.EffectiveOnly = TRUE;
  attrs.MaxMessageLength = ALPC_RECV_BUFFER_SIZE;
#ifdef _WIN64
  attrs.Reserved = 0;
#endif
  (void)::NtAlpcAcceptConnectPort(
      &accepted, server_port, 0, nullptr, &attrs, nullptr,
      reinterpret_cast<PPORT_MESSAGE>(&reply), nullptr, TRUE);
  if (accepted) {
    ::NtAlpcDisconnectPort(accepted, 0);
    ::NtClose(accepted);
  }
}

void handle_connection_request(HANDLE server_port, PPORT_MESSAGE request) {
  if (!request)
    return;

  // Dispatch by magic: state-mutation request vs join request.
  if (static_cast<unsigned short>(request->u1.s1.TotalLength) >=
          sizeof(PtyStateRequest)) {
    auto *state_req = reinterpret_cast<const PtyStateRequest *>(request);
    if (state_req->magic == PTY_STATE_REQUEST_MAGIC) {
      handle_state_request(server_port, request);
      return;
    }
  }

  auto *join_request = reinterpret_cast<const PtyJoinRequest *>(request);
  bool valid_request =
      static_cast<unsigned short>(request->u1.s1.TotalLength) >=
          sizeof(PtyJoinRequest) &&
      join_request->magic == PTY_JOIN_MAGIC &&
      join_request->version == PTY_JOIN_VERSION;

  PtyJoinReply reply = {};
  reply.header.u1.s1.DataLength =
      static_cast<CSHORT>(sizeof(PtyJoinReply) - sizeof(PORT_MESSAGE));
  reply.header.u1.s1.TotalLength = static_cast<CSHORT>(sizeof(PtyJoinReply));
  reply.header.MessageId = request->MessageId;
  reply.magic = PTY_JOIN_REPLY_MAGIC;
  reply.version = PTY_JOIN_REPLY_VERSION;
  reply.status = valid_request ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
  reply.handle_count = 0;

  if (valid_request) {
    DWORD requester_pid = static_cast<DWORD>(
        reinterpret_cast<uintptr_t>(request->ClientId.UniqueProcess));
    OwnedHandles owned = accept_join_request(join_request->id);
    if (!owned.reference || !owned.state_lock || !owned.state_section ||
        !owned.stdout_pipe) {
      reply.status = STATUS_OBJECT_NAME_NOT_FOUND;
    } else {
      OwnedHandles duplicate = duplicate_join_handles_for_pid(owned, requester_pid);
      if (!duplicate.reference || !duplicate.state_lock ||
          !duplicate.state_section || !duplicate.stdout_pipe) {
        close_owned_handles(duplicate);
        reply.status = STATUS_UNSUCCESSFUL;
      } else {
        reply.handle_count = PTY_JOIN_HANDLE_COUNT;
        reply.reference = static_cast<uint64_t>(
            reinterpret_cast<uintptr_t>(duplicate.reference));
        reply.state_lock = static_cast<uint64_t>(
            reinterpret_cast<uintptr_t>(duplicate.state_lock));
        reply.state_section = static_cast<uint64_t>(
            reinterpret_cast<uintptr_t>(duplicate.state_section));
        reply.stdout_pipe = static_cast<uint64_t>(
            reinterpret_cast<uintptr_t>(duplicate.stdout_pipe));
      }
    }
  }

  HANDLE accepted = nullptr;
  ALPC_PORT_ATTRIBUTES attrs = {};
  attrs.SecurityQos.Length = sizeof(SECURITY_QUALITY_OF_SERVICE);
  attrs.SecurityQos.ImpersonationLevel = SecurityIdentification;
  attrs.SecurityQos.ContextTrackingMode = 0;
  attrs.SecurityQos.EffectiveOnly = TRUE;
  attrs.MaxMessageLength = ALPC_RECV_BUFFER_SIZE;
#ifdef _WIN64
  attrs.Reserved = 0;
#endif
  NTSTATUS status = ::NtAlpcAcceptConnectPort(&accepted, server_port, 0,
                                              nullptr, &attrs, nullptr,
                                              reinterpret_cast<PPORT_MESSAGE>(&reply),
                                              nullptr, TRUE);
  if (!NT_SUCCESS(status))
    return;
  if (accepted) {
    ::NtAlpcDisconnectPort(accepted, 0);
    ::NtClose(accepted);
  }
}

void *service_thread_main(void *context) {
  auto *start = static_cast<ServiceThreadStartContext *>(context);
  HANDLE port = start ? start->port : nullptr;
  HANDLE ready_event = start ? start->ready_event : nullptr;
  if (start)
    page_free(start);
  if (ready_event) {
    LONG previous = 0;
    ::NtSetEvent(ready_event, &previous);
    ::NtClose(ready_event);
  }
  alignas(8) uint8_t recv_buffer[ALPC_RECV_BUFFER_SIZE];

  for (;;) {
    SIZE_T receive_length = sizeof(recv_buffer);
    NTSTATUS receive_status = ::NtAlpcSendWaitReceivePort(
        port, 0, nullptr, nullptr,
        reinterpret_cast<PPORT_MESSAGE>(recv_buffer), &receive_length, nullptr,
        nullptr);
    if (!NT_SUCCESS(receive_status)) {
      break;
    }

    auto *header = reinterpret_cast<PPORT_MESSAGE>(recv_buffer);
    CSHORT message_type =
        static_cast<CSHORT>(header->u2.s2.Type & static_cast<CSHORT>(0x0fff));
    if (message_type == LPC_CONNECTION_REQUEST) {
      handle_connection_request(port, header);
      continue;
    }

    if (message_type == LPC_PORT_CLOSED ||
        message_type == LPC_CLIENT_DIED)
      continue;
  }

  return nullptr;
}

NTSTATUS ensure_service_locked() {
  if (state().service_port)
    return STATUS_SUCCESS;
  NTSTATUS status = ensure_namespace_locked();
  if (!NT_SUCCESS(status))
    return status;
  uint64_t create_time = process_util::current_process_create_time();
  WCHAR name_buf[FULL_OBJECT_NAME_CAP] = {};
  size_t name_len =
      format_service_leaf_name(name_buf, FULL_OBJECT_NAME_CAP, current_pid(),
                               create_time);

  UNICODE_STRING name = {};
  OBJECT_ATTRIBUTES oa = {};
  init_named_attributes(&oa, &name, state().namespace_handle, name_buf,
                        name_len);

  ACL *dacl = build_service_port_dacl();
  if (!dacl)
    return STATUS_NO_MEMORY;
  auto free_dacl = cpp::make_scope_guard([&] { page_free(dacl); });
  SECURITY_DESCRIPTOR sd = {};
  build_service_port_sd(&sd, dacl);
  oa.SecurityDescriptor = &sd;

  ALPC_PORT_ATTRIBUTES attrs = {};
  attrs.Flags = ALPC_PORFLG_ALLOW_LPC_REQUESTS;
  attrs.SecurityQos.Length = sizeof(SECURITY_QUALITY_OF_SERVICE);
  attrs.SecurityQos.ImpersonationLevel = SecurityIdentification;
  attrs.SecurityQos.ContextTrackingMode = 0;
  attrs.SecurityQos.EffectiveOnly = TRUE;
  attrs.MaxMessageLength = ALPC_RECV_BUFFER_SIZE;
#ifdef _WIN64
  attrs.Reserved = 0;
#endif

  HANDLE port = nullptr;
  status = ::NtAlpcCreatePort(&port, &oa, &attrs);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  auto *start = static_cast<ServiceThreadStartContext *>(
      page_alloc(sizeof(ServiceThreadStartContext)));
  if (!start) {
    ::NtClose(port);
    return STATUS_NO_MEMORY;
  }
  start->port = port;
  start->ready_event = nullptr;

  HANDLE ready_event = nullptr;
  auto evt_oa = windows::internal_oa();
  status = ::NtCreateEvent(&ready_event, EVENT_ALL_ACCESS, &evt_oa,
                           SynchronizationEvent, FALSE);
  if (!NT_SUCCESS(status) || !ready_event) {
    page_free(start);
    ::NtClose(port);
    return NT_SUCCESS(status) ? STATUS_UNSUCCESSFUL : status;
  }
  start->ready_event = ready_event;

  Thread service_thread;
  int thread_result =
      service_thread.run(service_thread_main, start, nullptr,
                         Thread::DEFAULT_STACKSIZE, Thread::DEFAULT_GUARDSIZE,
                         true);
  if (thread_result != 0) {
    ::NtClose(ready_event);
    page_free(start);
    ::NtClose(port);
    return STATUS_UNSUCCESSFUL;
  }

  LARGE_INTEGER timeout = {};
  timeout.QuadPart = -10000000LL;
  status = ::NtWaitForSingleObject(ready_event, FALSE, &timeout);
  ::NtClose(ready_event);
  if (!NT_SUCCESS(status)) {
    ::NtClose(port);
    return status;
  }

  state().service_port = port;
  state().service_create_time = create_time;
  return STATUS_SUCCESS;
}

ErrorOr<OwnedHandles> connect_to_service_locked(uint32_t id, DWORD owner_pid,
                                                uint64_t owner_create_time,
                                                bool *retryable) {
  if (retryable)
    *retryable = false;
  NTSTATUS status = ensure_namespace_locked();
  if (!NT_SUCCESS(status))
    return Error(windows_util::ntstatus_to_errno(status));
  WCHAR name_buf[FULL_OBJECT_NAME_CAP] = {};
  size_t name_len = format_service_leaf_name(
      name_buf, FULL_OBJECT_NAME_CAP, owner_pid, owner_create_time);

  UNICODE_STRING name = {};
  OBJECT_ATTRIBUTES oa = {};
  init_named_attributes(&oa, &name, state().namespace_handle, name_buf,
                        name_len);

  ALPC_PORT_ATTRIBUTES attrs = {};
  attrs.SecurityQos.Length = sizeof(SECURITY_QUALITY_OF_SERVICE);
  attrs.SecurityQos.ImpersonationLevel = SecurityIdentification;
  attrs.SecurityQos.ContextTrackingMode = 0;
  attrs.SecurityQos.EffectiveOnly = TRUE;
  attrs.MaxMessageLength = ALPC_RECV_BUFFER_SIZE;
#ifdef _WIN64
  attrs.Reserved = 0;
#endif

  LARGE_INTEGER timeout = {};
  timeout.QuadPart = -2000000LL;
  union JoinConnectMessage {
    PtyJoinRequest request;
    PtyJoinReply reply;
  } message = {};
  message.request.header.u1.s1.DataLength =
      static_cast<CSHORT>(sizeof(PtyJoinRequest) - sizeof(PORT_MESSAGE));
  message.request.header.u1.s1.TotalLength =
      static_cast<CSHORT>(sizeof(PtyJoinRequest));
  message.request.magic = PTY_JOIN_MAGIC;
  message.request.version = PTY_JOIN_VERSION;
  message.request.id = id;

  SIZE_T receive_length = sizeof(message);
  HANDLE connection_port = nullptr;
  status = ::NtAlpcConnectPortEx(&connection_port, &oa, nullptr, &attrs,
                                 ALPC_MSGFLG_SYNC_REQUEST, nullptr,
                                 reinterpret_cast<PPORT_MESSAGE>(&message.request),
                                 &receive_length, nullptr, nullptr, &timeout);
  if (!NT_SUCCESS(status)) {
    if (retryable)
      *retryable = true;
    return Error(windows_util::ntstatus_to_errno(status));
  }
  auto close_connection = cpp::make_scope_guard([&] {
    if (connection_port) {
      ::NtAlpcDisconnectPort(connection_port, 0);
      ::NtClose(connection_port);
    }
  });

  const PtyJoinReply &reply = message.reply;
  if (receive_length < sizeof(PtyJoinReply) ||
      reply.magic != PTY_JOIN_REPLY_MAGIC ||
      reply.version != PTY_JOIN_REPLY_VERSION) {
    return Error(EIO);
  }
  if (!NT_SUCCESS(reply.status))
    return Error(windows_util::ntstatus_to_errno(reply.status));
  if (reply.handle_count != PTY_JOIN_HANDLE_COUNT)
    return Error(EIO);

  OwnedHandles handles = {};
  handles.reference =
      reinterpret_cast<HANDLE>(static_cast<uintptr_t>(reply.reference));
  handles.state_lock =
      reinterpret_cast<HANDLE>(static_cast<uintptr_t>(reply.state_lock));
  handles.state_section =
      reinterpret_cast<HANDLE>(static_cast<uintptr_t>(reply.state_section));
  handles.stdout_pipe =
      reinterpret_cast<HANDLE>(static_cast<uintptr_t>(reply.stdout_pipe));

  if (!handles.state_lock || !handles.state_section || !handles.reference ||
      !handles.stdout_pipe)
    return Error(EIO);
  return handles;
}

ErrorOr<OwnedHandles> request_handles_locked(uint32_t id, DWORD owner_pid,
                                             uint64_t owner_create_time) {

  HANDLE join_lock = nullptr;
  state().lock.lock();
  NTSTATUS status = open_join_lock_locked(&join_lock);
  state().lock.unlock();
  if (!NT_SUCCESS(status))
    return Error(windows_util::ntstatus_to_errno(status));
  auto close_join_lock = cpp::make_scope_guard([&] {
    if (join_lock) {
      (void)::NtReleaseMutant(join_lock, nullptr);
      ::NtClose(join_lock);
    }
  });
  status = ::NtWaitForSingleObject(join_lock, FALSE, nullptr);
  if (status != STATUS_SUCCESS && status != STATUS_ABANDONED)
    return Error(windows_util::ntstatus_to_errno(status));

  for (int attempt = 0; attempt < 2; ++attempt) {
    bool retryable = false;
    state().lock.lock();
    auto result =
        connect_to_service_locked(id, owner_pid, owner_create_time, &retryable);
    state().lock.unlock();
    if (result.has_value())
      return result;
    if (!retryable || attempt == 1)
      return result;
  }

  return Error(EIO);
}

ErrorOr<int> collect_tree_process_ids(DWORD **pids_out, DWORD *count_out) {
  if (!pids_out || !count_out)
    return Error(EINVAL);

  *pids_out = nullptr;
  *count_out = 0;

  init();
  HANDLE tree_job = nullptr;
  state().lock.lock();
  tree_job = state().tree_job;
  state().lock.unlock();
  if (!tree_job)
    return Error(ESRCH);

  SIZE_T capacity = sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) +
                    15 * sizeof(ULONG_PTR);
  for (;;) {
    auto *buffer = static_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST *>(
        page_alloc(capacity));
    if (!buffer)
      return Error(ENOMEM);

    ULONG return_length = 0;
    NTSTATUS status = ::NtQueryInformationJobObject(
        tree_job, JobObjectBasicProcessIdList, buffer,
        static_cast<ULONG>(capacity), &return_length);
    if (status == STATUS_BUFFER_TOO_SMALL || status == STATUS_BUFFER_OVERFLOW) {
      page_free(buffer);
      capacity = return_length > capacity ? return_length : capacity * 2;
      continue;
    }
    if (!NT_SUCCESS(status)) {
      page_free(buffer);
      return Error(windows_util::ntstatus_to_errno(status));
    }

    DWORD count = buffer->NumberOfProcessIdsInList;
    if (count == 0) {
      page_free(buffer);
      return 0;
    }

    DWORD *pids =
        static_cast<DWORD *>(page_alloc(static_cast<SIZE_T>(count) *
                                        sizeof(DWORD)));
    if (!pids) {
      page_free(buffer);
      return Error(ENOMEM);
    }

    for (DWORD i = 0; i < count; ++i)
      pids[i] = static_cast<DWORD>(buffer->ProcessIdList[i]);

    page_free(buffer);
    *pids_out = pids;
    *count_out = count;
    return 0;
  }
}

ErrorOr<uint64_t> query_process_create_time_by_pid(DWORD pid) {
  if (pid == current_pid())
    return process_util::current_process_create_time();

  HANDLE process = nullptr;
  NTSTATUS status =
      ::NtOpenProcessById(&process, PROCESS_QUERY_LIMITED_INFORMATION, pid);
  if (!NT_SUCCESS(status))
    return Error(windows_util::ntstatus_to_errno(status));

  uint64_t create_time = process_util::query_process_create_time(process);
  ::NtClose(process);
  if (create_time == 0)
    return Error(EIO);
  return create_time;
}

int parse_inherited_startup(PtyReserved2Ext *out) {
  process_utils::InheritedRuntimeDataView inherited = {};
  if (!process_utils::get_inherited_runtime_data(&inherited))
    return ENOENT;

  DWORD count = *reinterpret_cast<const DWORD *>(inherited.data);
  SIZE_T standard_size = sizeof(DWORD) + static_cast<SIZE_T>(count) *
                                             (1 + sizeof(HANDLE));
  SIZE_T aligned_offset = (standard_size + 7) & ~SIZE_T{7};
  SIZE_T pty_offset = aligned_offset + signal_state::RESERVED2_EXT_SIZE;
  if (inherited.size < pty_offset + PTY_RESERVED2_EXT_SIZE)
    return ENOENT;

  auto *ext = reinterpret_cast<const PtyReserved2Ext *>(
      inherited.data + pty_offset);
  if (ext->magic != LLVM_LIBC_PTY_RESERVED2_MAGIC ||
      ext->version != LLVM_LIBC_PTY_RESERVED2_VERSION)
    return ENOENT;

  *out = *ext;
  return 0;
}

void generate_tree_nonce(uint8_t (&nonce)[16]) {
  LUID first = {};
  LUID second = {};
  if (NT_SUCCESS(::NtAllocateLocallyUniqueId(&first)) &&
      NT_SUCCESS(::NtAllocateLocallyUniqueId(&second))) {
    __builtin_memcpy(nonce, &first, sizeof(first));
    __builtin_memcpy(nonce + sizeof(first), &second, sizeof(second));
    return;
  }

  uint64_t create_time = process_util::current_process_create_time();
  uint64_t pid = current_pid();
  __builtin_memcpy(nonce, &create_time, sizeof(create_time));
  __builtin_memcpy(nonce + sizeof(create_time), &pid, sizeof(pid));
}

void close_current_state_locked() {
  unmap_view_if_needed(&state().current_state_view);
  console_util::close_handle_if_valid(&state().current_state_section);
  console_util::close_handle_if_valid(&state().current_state_lock);
}

void close_allocator_locked() {
  unmap_view_if_needed(&state().alloc_state_view);
  console_util::close_handle_if_valid(&state().alloc_section);
  console_util::close_handle_if_valid(&state().alloc_lock);
}

int map_current_state_locked(const CurrentAttachment &attachment) {
  close_current_state_locked();
  if (attachment.pty_id == 0 || !attachment.state_lock ||
      !attachment.state_section) {
    state().current_attached_pty_id.store(0, cpp::MemoryOrder::RELEASE);
    return 0;
  }

  PtySharedState *view = nullptr;
  int err = validate_shared_state_view(attachment.state_section, attachment.pty_id,
                                       &view);
  if (err != 0)
    return err;

  state().current_state_lock = attachment.state_lock;
  state().current_state_section = attachment.state_section;
  state().current_state_view = view;
  state().current_attached_pty_id.store(attachment.pty_id,
                                        cpp::MemoryOrder::RELEASE);
  return 0;
}

template <typename Fn> int with_current_state_locked(Fn &&fn) {
  state().lock.lock();
  if (!state().current_state_view || !state().current_state_lock) {
    state().lock.unlock();
    return -ENOTTY;
  }

  HANDLE lock = state().current_state_lock;
  auto *view = static_cast<PtySharedState *>(state().current_state_view);
  state().lock.unlock();

  NTSTATUS status = wait_mutant(lock);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  int result = fn(*view);
  release_mutant(lock);
  return result;
}

int ensure_current_session_membership() {
  pid_t shared_sid = 0;
  int err = with_current_state_locked([&](const PtySharedState &shared) {
    shared_sid = shared.controlling_sid;
    return 0;
  });
  if (err < 0)
    return err;

  pid_t self_pid = static_cast<pid_t>(NtCurrentProcessId());
  pid_t current_sid = windows_syscalls::get_session_id();
  if (shared_sid != 0) {
    if (shared_sid == self_pid && current_sid != shared_sid) {
      auto sid_result = windows_syscalls::setsid();
      if (!sid_result.has_value())
        return -sid_result.error();
    }
    return 0;
  }

  if (current_sid != self_pid) {
    auto sid_result = windows_syscalls::setsid();
    if (!sid_result.has_value())
      return -sid_result.error();
    current_sid = sid_result.value();
  }

  pid_t pgid = windows_syscalls::getpgrp();
  return with_current_state_locked([&](PtySharedState &shared) {
    if (shared.controlling_sid == 0)
      shared.controlling_sid = current_sid;
    if (shared.foreground_pgrp == 0)
      shared.foreground_pgrp = pgid;
    return 0;
  });
}

} // namespace

void init() {
  auto &tree = state();
  tree.lock.lock();
  if (tree.initialized.load(cpp::MemoryOrder::ACQUIRE) != 0) {
    tree.lock.unlock();
    return;
  }

  PtyReserved2Ext inherited = {};
  if (parse_inherited_startup(&inherited) == 0) {
    __builtin_memcpy(tree.tree_nonce, inherited.tree_nonce, sizeof(tree.tree_nonce));
    tree.inherited_attach_flags = inherited.flags;
    tree.inherited_reference = inherited.attached_pty_reference;
    if (has_nonce_bytes(inherited.tree_nonce)) {
      CurrentAttachment attachment = {};
      attachment.pty_id = inherited.attached_pty_id;
      attachment.state_lock = inherited.attached_pty_state_lock;
      attachment.state_section = inherited.attached_pty_state_section;
      (void)map_current_state_locked(attachment);
    }
  } else {
    generate_tree_nonce(tree.tree_nonce);
    tree.tree_job = nullptr;
    tree.inherited_attach_flags = 0;
    tree.inherited_reference = nullptr;
  }

  (void)ensure_tree_job_locked();

  tree.initialized.store(1, cpp::MemoryOrder::RELEASE);
  tree.lock.unlock();
}

void fini() {
  auto &tree = state();
  tree.lock.lock();
  close_allocator_locked();
  close_current_state_locked();
  console_util::close_handle_if_valid(&tree.tree_job);
  console_util::close_handle_if_valid(&tree.inherited_reference);
  console_util::close_handle_if_valid(&tree.service_port);
  console_util::close_handle_if_valid(&tree.namespace_handle);
  if (tree.boundary_descriptor) {
    ::RtlDeleteBoundaryDescriptor(
        reinterpret_cast<POBJECT_BOUNDARY_DESCRIPTOR>(tree.boundary_descriptor));
    tree.boundary_descriptor = nullptr;
  }
  tree.lock.unlock();

  owned_pty_lock.lock();
  while (owned_pty_head) {
    OwnedPtyEntry *entry = owned_pty_head;
    owned_pty_head = entry->next;
    close_owned_handles(entry->handles);
    page_free(entry);
  }
  owned_pty_lock.unlock();
}

void fork_reinit() {
  auto &tree = state();
  tree.lock.reset_for_fork();
  tree.lock.lock();
  // Unmap CoW views (valid VA operation in the child), but don't NtClose
  // the section/lock handles — they were created with internal_oa()
  // (non-inheritable) and don't exist in the child's handle table.
  unmap_view_if_needed(&tree.alloc_state_view);
  tree.alloc_section = nullptr;
  tree.alloc_lock = nullptr;
  unmap_view_if_needed(&tree.current_state_view);
  tree.current_state_section = nullptr;
  tree.current_state_lock = nullptr;
  tree.current_attached_pty_id.store(0, cpp::MemoryOrder::RELAXED);
  tree.tree_job = nullptr;
  tree.inherited_attach_flags = 0;
  tree.inherited_reference = nullptr;
  tree.service_port = nullptr;
  tree.service_create_time = 0;
  tree.namespace_handle = nullptr;
  tree.boundary_descriptor = nullptr;
  tree.initialized.store(0, cpp::MemoryOrder::RELAXED);
  tree.lock.unlock();

  owned_pty_lock.reset_for_fork();
  owned_pty_head = nullptr;
}

void copy_tree_nonce(uint8_t (&out)[16]) {
  init();
  __builtin_memcpy(out, state().tree_nonce, sizeof(out));
}

bool has_tree_nonce() {
  init();
  return has_nonce_bytes(state().tree_nonce);
}

ErrorOr<uint32_t> allocate_pty_id() {
  init();
  HANDLE alloc_lock = nullptr;
  PtyAllocState *alloc_state = nullptr;
  state().lock.lock();
  NTSTATUS lock_status = open_alloc_lock_locked(&alloc_lock);
  NTSTATUS view_status = STATUS_SUCCESS;
  if (NT_SUCCESS(lock_status))
    view_status = alloc_state_view_locked(&state().alloc_section, &alloc_state);
  state().lock.unlock();

  if (!NT_SUCCESS(lock_status) || !NT_SUCCESS(view_status)) {
    if (!NT_SUCCESS(lock_status))
      return Error(windows_util::ntstatus_to_errno(lock_status));
    return Error(windows_util::ntstatus_to_errno(view_status));
  }

  NTSTATUS status = wait_mutant(alloc_lock);
  if (!NT_SUCCESS(status))
    return Error(windows_util::ntstatus_to_errno(status));
  auto cleanup =
      cpp::make_scope_guard([&] { release_mutant(alloc_lock); });

  uint32_t id = alloc_state->next_id.fetch_add(1, cpp::MemoryOrder::ACQ_REL);
  if (id == 0)
    return Error(ENOSPC);
  return id;
}

ErrorOr<int> create_shared_state(uint32_t id, HANDLE *state_lock,
                                 HANDLE *state_section,
                                 PtySharedState **state_view) {
  if (!state_lock || !state_section || !state_view || id == 0)
    return Error(EINVAL);

  *state_lock = nullptr;
  *state_section = nullptr;
  *state_view = nullptr;

  init();
  state().lock.lock();
  int err =
      create_shared_state_locked(id, state_lock, state_section, state_view);
  state().lock.unlock();
  if (err != 0)
    return Error(err);
  return 0;
}

int duplicate_state_handles(HANDLE source_lock, HANDLE source_section,
                            HANDLE *state_lock, HANDLE *state_section,
                            bool inherit) {
  if (!source_lock || !source_section || !state_lock || !state_section)
    return EINVAL;

  *state_lock = nullptr;
  *state_section = nullptr;

  NTSTATUS status = inherit ? console_util::duplicate_inherited(source_lock,
                                                                state_lock)
                            : console_util::duplicate_noninherited(source_lock,
                                                                   state_lock);
  if (!NT_SUCCESS(status))
    return windows_util::ntstatus_to_errno(status);

  // Slaves get read-only section access. Writes go through ALPC to master.
  constexpr ACCESS_MASK SECTION_READ_ONLY =
      SECTION_MAP_READ | SECTION_QUERY;
  ULONG inherit_attr = inherit ? OBJ_INHERIT : 0u;
  status = ::NtDuplicateObject(NtCurrentProcess(), source_section,
                               NtCurrentProcess(), state_section,
                               SECTION_READ_ONLY, inherit_attr, 0);
  if (!NT_SUCCESS(status)) {
    console_util::close_handle_if_valid(state_lock);
    return windows_util::ntstatus_to_errno(status);
  }

  return 0;
}

int map_shared_state_view(HANDLE state_section, uint32_t expected_id,
                          PtySharedState **state_view) {
  return validate_shared_state_view(state_section, expected_id, state_view);
}

void close_shared_state(HANDLE *state_lock, HANDLE *state_section,
                        PtySharedState **state_view) {
  if (state_view && *state_view) {
    void *view = *state_view;
    unmap_view_if_needed(&view);
    *state_view = nullptr;
  }
  console_util::close_handle_if_valid(state_section);
  console_util::close_handle_if_valid(state_lock);
}

ErrorOr<int> register_owned_pty(uint32_t id, const OwnedHandles &handles) {
  if (id == 0 || !handles.state_lock || !handles.state_section ||
      !handles.reference || !handles.connection || !handles.input ||
      !handles.output || !handles.error || !handles.stdout_pipe)
    return Error(EINVAL);

  init();
  state().lock.lock();
  NTSTATUS status = ensure_service_locked();
  state().lock.unlock();
  if (!NT_SUCCESS(status))
    return Error(windows_util::ntstatus_to_errno(status));

  OwnedHandles duplicate =
      duplicate_owned_handles_for_pid(handles, current_pid());
  if (!duplicate.reference)
    return Error(EIO);

  auto *entry = static_cast<OwnedPtyEntry *>(page_alloc(sizeof(OwnedPtyEntry)));
  if (!entry) {
    close_owned_handles(duplicate);
    return Error(ENOMEM);
  }

  entry->id = id;
  entry->handles = duplicate;
  entry->next = nullptr;

  owned_pty_lock.lock();
  OwnedPtyEntry *existing = find_owned_locked(id);
  if (existing) {
    owned_pty_lock.unlock();
    close_owned_handles(duplicate);
    page_free(entry);
    return Error(EEXIST);
  }
  entry->next = owned_pty_head;
  owned_pty_head = entry;
  owned_pty_lock.unlock();
  return 0;
}

void unregister_owned_pty(uint32_t id) {
  if (id == 0)
    return;

  owned_pty_lock.lock();
  OwnedPtyEntry **link = &owned_pty_head;
  while (*link && (*link)->id != id)
    link = &(*link)->next;

  OwnedPtyEntry *entry = *link;
  if (entry)
    *link = entry->next;
  owned_pty_lock.unlock();

  if (!entry)
    return;
  close_owned_handles(entry->handles);
  page_free(entry);
}

ErrorOr<OwnedHandles> duplicate_local_owned_handles(uint32_t id) {
  if (id == 0)
    return Error(ENOENT);

  owned_pty_lock.lock();
  OwnedPtyEntry *entry = find_owned_locked(id);
  OwnedHandles handles = entry ? entry->handles : OwnedHandles{};
  owned_pty_lock.unlock();
  if (!handles.state_lock || !handles.state_section || !handles.reference)
    return Error(ENOENT);

  OwnedHandles duplicate =
      duplicate_owned_handles_for_pid(handles, current_pid());
  if (!duplicate.reference)
    return Error(EIO);
  return duplicate;
}

ErrorOr<OwnedHandles> join_handles(uint32_t id) {
  if (id == 0)
    return Error(ENOENT);

  auto local_result = duplicate_local_owned_handles(id);
  if (local_result.has_value())
    return local_result;
  if (local_result.error() != ENOENT)
    return Error(local_result.error());

  DWORD *pids = nullptr;
  DWORD count = 0;
  auto collect_result = collect_tree_process_ids(&pids, &count);
  if (!collect_result.has_value())
    return Error(collect_result.error());
  auto cleanup_pids = cpp::make_scope_guard([&] {
    if (pids)
      page_free(pids);
  });

  int first_error = ENOENT;
  for (DWORD i = 0; i < count; ++i) {
    DWORD pid = pids[i];
    auto create_time_result = query_process_create_time_by_pid(pid);
    if (!create_time_result.has_value())
      continue;

    auto request_result =
        request_handles_locked(id, pid, create_time_result.value());
    if (!request_result.has_value()) {
      int err = request_result.error();
      if (err != ENOENT && err != ESRCH)
        first_error = err;
      continue;
    }

    OwnedHandles handles = request_result.value();
    PtySharedState *view = nullptr;
    int view_err = validate_shared_state_view(handles.state_section, id, &view);
    if (view_err != 0) {
      close_owned_handles(handles);
      if (first_error == ENOENT)
        first_error = view_err;
      continue;
    }

    auto cleanup_view = cpp::make_scope_guard([&] {
      if (view) {
        void *raw_view = view;
        unmap_view_if_needed(&raw_view);
        view = nullptr;
      }
    });

    bool locked = false;
    LARGE_INTEGER zero_timeout = {};
    NTSTATUS status =
        ::NtWaitForSingleObject(handles.state_lock, FALSE, &zero_timeout);
    if (status == STATUS_SUCCESS || status == STATUS_ABANDONED) {
      locked = true;
    } else if (status != STATUS_TIMEOUT) {
      close_owned_handles(handles);
      if (first_error == ENOENT)
        first_error = windows_util::ntstatus_to_errno(status);
      continue;
    }

    uint16_t flags = view->flags;
    if (locked)
      release_mutant(handles.state_lock);

    if ((flags & PTY_SHARED_FLAG_UNLOCKED) == 0) {
      close_owned_handles(handles);
      first_error = EACCES;
      continue;
    }
    if (flags & PTY_SHARED_FLAG_HUNGUP) {
      close_owned_handles(handles);
      first_error = ENXIO;
      continue;
    }

    return handles;
  }

  return Error(first_error);
}

uint32_t current_attached_pty_id() {
  init();
  return state().current_attached_pty_id.load(cpp::MemoryOrder::ACQUIRE);
}

bool has_current_attached_pty() { return current_attached_pty_id() != 0; }

HANDLE take_inherited_reference() {
  init();
  auto &tree = state();
  tree.lock.lock();
  HANDLE reference = tree.inherited_reference;
  tree.inherited_reference = nullptr;
  tree.lock.unlock();
  return reference;
}

uint16_t inherited_attach_flags() {
  init();
  auto &tree = state();
  tree.lock.lock();
  uint16_t flags = tree.inherited_attach_flags;
  tree.lock.unlock();
  return flags;
}

int duplicate_current_attachment(CurrentAttachment *attachment, bool inherit) {
  init();
  if (!attachment)
    return EINVAL;

  *attachment = {};
  state().lock.lock();
  attachment->pty_id =
      state().current_attached_pty_id.load(cpp::MemoryOrder::ACQUIRE);
  HANDLE source_lock = state().current_state_lock;
  HANDLE source_section = state().current_state_section;
  state().lock.unlock();

  if (attachment->pty_id == 0 || !source_lock || !source_section)
    return 0;

  int err = duplicate_state_handles(source_lock, source_section,
                                    &attachment->state_lock,
                                    &attachment->state_section, inherit);
  if (err != 0) {
    *attachment = {};
    return err;
  }
  return 0;
}

void release_current_attachment(CurrentAttachment *attachment) {
  if (!attachment)
    return;
  console_util::close_handle_if_valid(&attachment->state_section);
  console_util::close_handle_if_valid(&attachment->state_lock);
  attachment->pty_id = 0;
}

int set_current_attached_pty(const CurrentAttachment &attachment) {
  init();
  CurrentAttachment local = {};
  if (attachment.pty_id != 0) {
    int err = duplicate_state_handles(attachment.state_lock,
                                      attachment.state_section,
                                      &local.state_lock,
                                      &local.state_section, false);
    if (err != 0)
      return err;
    local.pty_id = attachment.pty_id;
  }

  state().lock.lock();
  int err = map_current_state_locked(local);
  state().lock.unlock();
  if (err != 0) {
    release_current_attachment(&local);
    return err;
  }
  return 0;
}

void clear_current_attached_pty() {
  init();
  state().lock.lock();
  CurrentAttachment empty = {};
  (void)map_current_state_locked(empty);
  state().lock.unlock();
}

// Lock-free read of PtySharedState using the seqlock. No kernel transitions.
// The view is PAGE_READONLY for slaves; the seqlock ensures consistency.
template <typename Fn>
int with_current_state_seqlock(Fn &&fn) {
  state().lock.lock();
  auto *view = static_cast<const PtySharedState *>(state().current_state_view);
  state().lock.unlock();
  if (!view)
    return -ENOTTY;

  for (;;) {
    uint32_t seq = seqlock_read_begin(view);
    int result = fn(*view);
    if (!seqlock_read_retry(view, seq))
      return result;
    // Writer was active — retry.
    spin_wait::relax_processor();
  }
}

int current_get_attr(struct termios *attrs) {
  if (!attrs)
    return -EINVAL;
  return with_current_state_seqlock([&](const PtySharedState &shared) {
    *attrs = shared.attrs;
    return 0;
  });
}

// Send a state-mutation request to the master process via ALPC.
// The slave cannot write shared memory directly (read-only mapping).
int send_state_request_to_owner(const PtyStateRequest &req, uint64_t owner_pid,
                                uint64_t owner_create_time) {
  if (!owner_pid)
    return -ENOTTY;

  // Connect to the master's ALPC service port.
  NTSTATUS ns_status = ensure_namespace_locked();
  if (!NT_SUCCESS(ns_status))
    return -windows_util::ntstatus_to_errno(ns_status);

  WCHAR name_buf[FULL_OBJECT_NAME_CAP] = {};
  size_t name_len = format_service_leaf_name(
      name_buf, FULL_OBJECT_NAME_CAP, static_cast<DWORD>(owner_pid),
      owner_create_time);

  UNICODE_STRING name = {};
  OBJECT_ATTRIBUTES oa = {};
  init_named_attributes(&oa, &name, state().namespace_handle, name_buf,
                        name_len);

  ALPC_PORT_ATTRIBUTES port_attrs = {};
  port_attrs.SecurityQos.Length = sizeof(SECURITY_QUALITY_OF_SERVICE);
  port_attrs.SecurityQos.ImpersonationLevel = SecurityIdentification;
  port_attrs.SecurityQos.ContextTrackingMode = 0;
  port_attrs.SecurityQos.EffectiveOnly = TRUE;
  port_attrs.MaxMessageLength = ALPC_RECV_BUFFER_SIZE;
#ifdef _WIN64
  port_attrs.Reserved = 0;
#endif

  LARGE_INTEGER timeout = {};
  timeout.QuadPart = -5000000LL; // 500ms

  union StateConnectMessage {
    PtyStateRequest request;
    PtyStateReply reply;
  } message = {};
  message.request = req;

  SIZE_T receive_length = sizeof(message);
  HANDLE connection_port = nullptr;
  NTSTATUS status = ::NtAlpcConnectPortEx(
      &connection_port, &oa, nullptr, &port_attrs, ALPC_MSGFLG_SYNC_REQUEST,
      nullptr, reinterpret_cast<PPORT_MESSAGE>(&message.request),
      &receive_length, nullptr, nullptr, &timeout);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  if (connection_port) {
    ::NtAlpcDisconnectPort(connection_port, 0);
    ::NtClose(connection_port);
  }

  const PtyStateReply &reply = message.reply;
  if (receive_length < sizeof(PtyStateReply) ||
      reply.magic != PTY_STATE_REPLY_MAGIC ||
      reply.version != PTY_STATE_REPLY_VERSION)
    return -EIO;
  if (!NT_SUCCESS(reply.status))
    return -windows_util::ntstatus_to_errno(reply.status);
  return 0;
}

int send_current_state_request(const PtyStateRequest &req) {
  state().lock.lock();
  auto *view = static_cast<const PtySharedState *>(state().current_state_view);
  if (!view) {
    state().lock.unlock();
    return -ENOTTY;
  }
  uint64_t owner_pid = view->owner_pid;
  uint64_t owner_create_time = view->owner_create_time;
  state().lock.unlock();
  return send_state_request_to_owner(req, owner_pid, owner_create_time);
}

int query_state_owner(HANDLE state_section, uint32_t expected_id,
                      uint64_t *owner_pid, uint64_t *owner_create_time) {
  if (!state_section || !owner_pid || !owner_create_time)
    return -EINVAL;

  PtySharedState *view = nullptr;
  int err = validate_shared_state_view(state_section, expected_id, &view);
  if (err != 0)
    return -err;

  *owner_pid = view->owner_pid;
  *owner_create_time = view->owner_create_time;

  void *raw_view = view;
  unmap_view_if_needed(&raw_view);
  return 0;
}

int current_set_attr(const struct termios *attrs) {
  if (!attrs)
    return -EINVAL;

  // Check if we are the master (can write directly).
  uint32_t pty_id =
      state().current_attached_pty_id.load(cpp::MemoryOrder::ACQUIRE);
  owned_pty_lock.lock();
  OwnedPtyEntry *entry = find_owned_locked(pty_id);
  owned_pty_lock.unlock();

  if (entry) {
    // Master path: write directly under mutant with seqlock.
    return with_current_state_locked([&](PtySharedState &shared) {
      seqlock_write_begin(&shared);
      shared.attrs = *attrs;
      seqlock_write_end(&shared);
      return 0;
    });
  }

  // Slave path: send ALPC request to master.
  PtyStateRequest req = {};
  req.header.u1.s1.DataLength =
      static_cast<CSHORT>(sizeof(PtyStateRequest) - sizeof(PORT_MESSAGE));
  req.header.u1.s1.TotalLength = static_cast<CSHORT>(sizeof(PtyStateRequest));
  req.magic = PTY_STATE_REQUEST_MAGIC;
  req.version = PTY_STATE_REQUEST_VERSION;
  req.op = PTY_STATE_OP_SET_ATTR;
  req.pty_id = pty_id;
  req.payload.attrs = *attrs;
  return send_current_state_request(req);
}

int set_attr(uint32_t pty_id, HANDLE state_section, const struct termios *attrs) {
  if (pty_id == 0 || !state_section || !attrs)
    return -EINVAL;

  owned_pty_lock.lock();
  OwnedPtyEntry *entry = find_owned_locked(pty_id);
  HANDLE state_lock = entry ? entry->handles.state_lock : nullptr;
  HANDLE owned_state_section = entry ? entry->handles.state_section : nullptr;
  owned_pty_lock.unlock();

  if (state_lock && owned_state_section) {
    PtySharedState *view = nullptr;
    int err = validate_shared_state_view(owned_state_section, pty_id, &view);
    if (err != 0)
      return -err;

    NTSTATUS status = wait_mutant(state_lock);
    if (!NT_SUCCESS(status)) {
      void *raw_view = view;
      unmap_view_if_needed(&raw_view);
      return -windows_util::ntstatus_to_errno(status);
    }

    seqlock_write_begin(view);
    view->attrs = *attrs;
    seqlock_write_end(view);

    release_mutant(state_lock);
    void *raw_view = view;
    unmap_view_if_needed(&raw_view);
    return 0;
  }

  uint64_t owner_pid = 0;
  uint64_t owner_create_time = 0;
  int err = query_state_owner(state_section, pty_id, &owner_pid,
                              &owner_create_time);
  if (err < 0)
    return err;

  PtyStateRequest req = {};
  req.header.u1.s1.DataLength =
      static_cast<CSHORT>(sizeof(PtyStateRequest) - sizeof(PORT_MESSAGE));
  req.header.u1.s1.TotalLength = static_cast<CSHORT>(sizeof(PtyStateRequest));
  req.magic = PTY_STATE_REQUEST_MAGIC;
  req.version = PTY_STATE_REQUEST_VERSION;
  req.op = PTY_STATE_OP_SET_ATTR;
  req.pty_id = pty_id;
  req.payload.attrs = *attrs;
  return send_state_request_to_owner(req, owner_pid, owner_create_time);
}

int seed_initial_session(HANDLE state_lock, HANDLE state_section, pid_t sid,
                         pid_t pgid) {
  if (!state_lock || !state_section || sid <= 0 || pgid <= 0)
    return -EINVAL;

  PtySharedState *view = nullptr;
  int err = validate_shared_state_view(state_section, 0, &view);
  if (err != 0)
    return -err;
  auto cleanup = cpp::make_scope_guard([&] {
    if (view) {
      void *raw_view = view;
      unmap_view_if_needed(&raw_view);
      view = nullptr;
    }
  });

  NTSTATUS status = wait_mutant(state_lock);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  seqlock_write_begin(view);
  if (view->controlling_sid == 0)
    view->controlling_sid = sid;
  if (view->foreground_pgrp == 0)
    view->foreground_pgrp = pgid;
  seqlock_write_end(view);

  release_mutant(state_lock);
  return 0;
}

int current_get_sid(pid_t *sid) {
  if (!sid)
    return -EINVAL;
  int err = ensure_current_session_membership();
  if (err < 0)
    return err;
  return with_current_state_seqlock([&](const PtySharedState &shared) {
    *sid = shared.controlling_sid;
    return 0;
  });
}

int current_get_foreground_pgrp(pid_t *pgid) {
  if (!pgid)
    return -EINVAL;
  int err = ensure_current_session_membership();
  if (err < 0)
    return err;
  return with_current_state_seqlock([&](const PtySharedState &shared) {
    *pgid = shared.foreground_pgrp;
    return 0;
  });
}

int current_set_foreground_pgrp(pid_t pgid) {
  if (pgid <= 0)
    return -EINVAL;

  uint32_t pty_id =
      state().current_attached_pty_id.load(cpp::MemoryOrder::ACQUIRE);
  owned_pty_lock.lock();
  OwnedPtyEntry *entry = find_owned_locked(pty_id);
  owned_pty_lock.unlock();

  if (entry) {
    // Master path: write directly under mutant with seqlock.
    return with_current_state_locked([&](PtySharedState &shared) {
      seqlock_write_begin(&shared);
      shared.foreground_pgrp = pgid;
      seqlock_write_end(&shared);
      return 0;
    });
  }

  // Slave path: send ALPC request to master.
  PtyStateRequest req = {};
  req.header.u1.s1.DataLength =
      static_cast<CSHORT>(sizeof(PtyStateRequest) - sizeof(PORT_MESSAGE));
  req.header.u1.s1.TotalLength = static_cast<CSHORT>(sizeof(PtyStateRequest));
  req.magic = PTY_STATE_REQUEST_MAGIC;
  req.version = PTY_STATE_REQUEST_VERSION;
  req.op = PTY_STATE_OP_SET_FOREGROUND_PGRP;
  req.pty_id = pty_id;
  req.payload.pgid = pgid;
  return send_current_state_request(req);
}

int set_foreground_pgrp(uint32_t pty_id, HANDLE state_section, pid_t pgid) {
  if (pty_id == 0 || !state_section || pgid <= 0)
    return -EINVAL;

  owned_pty_lock.lock();
  OwnedPtyEntry *entry = find_owned_locked(pty_id);
  HANDLE state_lock = entry ? entry->handles.state_lock : nullptr;
  HANDLE owned_state_section = entry ? entry->handles.state_section : nullptr;
  owned_pty_lock.unlock();

  if (state_lock && owned_state_section) {
    PtySharedState *view = nullptr;
    int err = validate_shared_state_view(owned_state_section, pty_id, &view);
    if (err != 0)
      return -err;

    NTSTATUS status = wait_mutant(state_lock);
    if (!NT_SUCCESS(status)) {
      void *raw_view = view;
      unmap_view_if_needed(&raw_view);
      return -windows_util::ntstatus_to_errno(status);
    }

    seqlock_write_begin(view);
    view->foreground_pgrp = pgid;
    seqlock_write_end(view);

    release_mutant(state_lock);
    void *raw_view = view;
    unmap_view_if_needed(&raw_view);
    return 0;
  }

  uint64_t owner_pid = 0;
  uint64_t owner_create_time = 0;
  int err = query_state_owner(state_section, pty_id, &owner_pid,
                              &owner_create_time);
  if (err < 0)
    return err;

  PtyStateRequest req = {};
  req.header.u1.s1.DataLength =
      static_cast<CSHORT>(sizeof(PtyStateRequest) - sizeof(PORT_MESSAGE));
  req.header.u1.s1.TotalLength = static_cast<CSHORT>(sizeof(PtyStateRequest));
  req.magic = PTY_STATE_REQUEST_MAGIC;
  req.version = PTY_STATE_REQUEST_VERSION;
  req.op = PTY_STATE_OP_SET_FOREGROUND_PGRP;
  req.pty_id = pty_id;
  req.payload.pgid = pgid;
  return send_state_request_to_owner(req, owner_pid, owner_create_time);
}

int set_session_controller(uint32_t pty_id, HANDLE state_section, pid_t sid,
                           pid_t pgid, pid_t controller_pid,
                           uint64_t controller_create_time) {
  if (pty_id == 0 || !state_section || sid <= 0 || pgid <= 0 ||
      controller_pid <= 0)
    return -EINVAL;

  owned_pty_lock.lock();
  OwnedPtyEntry *entry = find_owned_locked(pty_id);
  HANDLE state_lock = entry ? entry->handles.state_lock : nullptr;
  HANDLE owned_state_section = entry ? entry->handles.state_section : nullptr;
  owned_pty_lock.unlock();

  if (state_lock && owned_state_section) {
    PtySharedState *view = nullptr;
    int err =
        validate_shared_state_view(owned_state_section, pty_id, &view);
    if (err != 0)
      return -err;

    NTSTATUS status = wait_mutant(state_lock);
    if (!NT_SUCCESS(status)) {
      void *raw_view = view;
      unmap_view_if_needed(&raw_view);
      return -windows_util::ntstatus_to_errno(status);
    }

    seqlock_write_begin(view);
    view->controlling_sid = sid;
    view->foreground_pgrp = pgid;
    view->controller_pid = controller_pid;
    view->controller_create_time = controller_create_time;
    seqlock_write_end(view);

    release_mutant(state_lock);
    void *raw_view = view;
    unmap_view_if_needed(&raw_view);
    return 0;
  }

  uint64_t owner_pid = 0;
  uint64_t owner_create_time = 0;
  int err = query_state_owner(state_section, pty_id, &owner_pid,
                              &owner_create_time);
  if (err < 0)
    return err;

  PtyStateRequest req = {};
  req.header.u1.s1.DataLength =
      static_cast<CSHORT>(sizeof(PtyStateRequest) - sizeof(PORT_MESSAGE));
  req.header.u1.s1.TotalLength = static_cast<CSHORT>(sizeof(PtyStateRequest));
  req.magic = PTY_STATE_REQUEST_MAGIC;
  req.version = PTY_STATE_REQUEST_VERSION;
  req.op = PTY_STATE_OP_SET_SESSION_CONTROLLER;
  req.pty_id = pty_id;
  req.payload.controller.controller_create_time = controller_create_time;
  req.payload.controller.controlling_sid = sid;
  req.payload.controller.foreground_pgrp = pgid;
  req.payload.controller.controller_pid = controller_pid;
  req.payload.controller.reserved = 0;
  return send_state_request_to_owner(req, owner_pid, owner_create_time);
}

int current_get_winsize(struct winsize *ws) {
  if (!ws)
    return -EINVAL;
  return with_current_state_seqlock([&](const PtySharedState &shared) {
    *ws = shared.winsize;
    return 0;
  });
}

int current_get_sync_snapshot(SyncSnapshot *snapshot) {
  if (!snapshot)
    return -EINVAL;
  return with_current_state_seqlock([&](const PtySharedState &shared) {
    snapshot->attrs = shared.attrs;
    snapshot->controlling_sid = shared.controlling_sid;
    snapshot->foreground_pgrp = shared.foreground_pgrp;
    return 0;
  });
}
} // namespace pty_tree
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
void LIBC_NAMESPACE::internal::pty_tree_fork_reinit() {
  LIBC_NAMESPACE::internal::pty_tree::fork_reinit();
}
