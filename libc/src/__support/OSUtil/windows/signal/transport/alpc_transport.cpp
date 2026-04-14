//===-- ALPC transport (Layer 2c) — cross-process signal delivery ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Full ALPC-based cross-process signal delivery. Replaces the PE-export-walking
// APC mechanism with kernel-native IPC:
//
//   - Private namespace isolation (SID + medium IL boundary descriptor)
//   - ALPC port with DACL (Creator Owner + Administrators)
//   - Reactor-driven receive via IOCP completion (no dedicated thread)
//   - One-shot connection-request delivery (no long-lived signal comm ports)
//   - Kernel-attested sender identity (ClientId + create_time revalidation)
//   - Three-tier permission model (DACL → sender-side → receiver-side)
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/signal/transport/alpc_transport.h"

#include "src/__support/OSUtil/windows/reactor/reactor.h"

#include "hdr/errno_macros.h"
#include "hdr/signal_macros.h"
#include "hdr/types/pid_t.h"
#include "hdr/types/siginfo_t.h"
#include "hdr/types/uid_t.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/nt/nt_ipc.h"
#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/OSUtil/windows/nt/nt_security.h"
#include "src/__support/OSUtil/windows/process/sid_utils.h"
#include "src/__support/OSUtil/windows/signal/dispatch/dispatch_fwd.h"
#include "src/__support/OSUtil/windows/signal/pending/pending_storage.h"
#include "src/__support/OSUtil/windows/signal/pending/sigqueue_pool.h"
#include "src/__support/OSUtil/windows/signal/signal_types.h"
#include "src/__support/OSUtil/windows/alloc/page_alloc.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {

using internal::page_alloc;
using internal::page_free;
namespace reactor = internal::reactor;

namespace {

// =========================================================================
// Constants
// =========================================================================

// Private namespace name. All NT-POSIX processes of the same user share
// this namespace. Port names inside are per-process (PID + create_time).
[[maybe_unused]] constexpr WCHAR NAMESPACE_NAME[] = u"NtPosixSignals";

// Port name prefix inside the namespace.
constexpr WCHAR PORT_NAME_PREFIX[] = u"NtPosixSig-";

constexpr uint32_t SIGNAL_REPLY_MAGIC = 0x52534947; // "GISR" in LE
constexpr uint16_t SIGNAL_REPLY_VERSION = 1;

// Maximum port name length: prefix + PID (10 digits) + '-' + create_time (16 hex) + NUL.
inline constexpr int PORT_NAME_BUF_LEN = 64;

// Maximum size for message receive buffer (header + payload + attribute space).
inline constexpr SIZE_T RECV_BUF_SIZE = 256;

// Privilege LUID for SeDebugPrivilege — root/CAP_KILL equivalent.
inline constexpr ULONG SE_DEBUG_PRIVILEGE = 20;

struct SignalReply {
  PORT_MESSAGE header;
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  NTSTATUS status;
};

// Signals deliberately stay on ALPC's small-message path: no data views,
// no handle transfer, and no receiver-side mapped section lifetime to manage.
union SignalConnectMessage {
  SignalMessage request;
  SignalReply reply;
};

// =========================================================================
// Transport-private state
//
// These fields are implementation details of the ALPC transport lifecycle.
// They are NOT in the PCB because no other subsystem needs them:
//   - port_sd/port_dacl/port_attrs: reused only during fork_reinit to
//     recreate the port with the same security descriptor.
//   - reactor_token: ephemeral registration handle, invalidated on fork.
//
// The observable process-wide ALPC state lives in g_pcb.signal_alpc.
// =========================================================================

static SECURITY_DESCRIPTOR g_port_sd = {};
static ACL *g_port_dacl = nullptr;
static ALPC_PORT_ATTRIBUTES g_port_attrs = {};
static reactor::ReactorToken g_reactor_token = reactor::INVALID_TOKEN;

// =========================================================================
// Small-message protocol layout validation
// =========================================================================

static_assert(sizeof(pid_t) == sizeof(int32_t),
              "pid_t must be 32-bit for signal transport naming");
static_assert(sizeof(SignalConnectMessage) == sizeof(SignalMessage),
              "Signal reply must fit in the request buffer");

int format_port_name(WCHAR *buf, int buf_len, pid_t pid,
                     uint64_t create_time);
void init_unicode_string(UNICODE_STRING *us, WCHAR *buf, int char_len);

// =========================================================================
// Helper: get current PID from TEB (fast, no syscall)
// =========================================================================

pid_t get_current_pid() {
  pid_t pid;
#ifdef __x86_64__
  __asm__ __volatile__("movl %%gs:0x40, %0" : "=r"(pid));
#elif defined(__aarch64__)
  __asm__ __volatile__("ldr %w0, [x18, #0x40]" : "=r"(pid));
#else
#error "Unsupported architecture"
#endif
  return pid;
}

// =========================================================================
// Helper: get current TID from TEB
// =========================================================================

[[maybe_unused]] DWORD get_current_tid() {
  DWORD tid;
#ifdef __x86_64__
  __asm__ __volatile__("movl %%gs:0x48, %0" : "=r"(tid));
#elif defined(__aarch64__)
  __asm__ __volatile__("ldr %w0, [x18, #0x48]" : "=r"(tid));
#else
#error "Unsupported architecture"
#endif
  return tid;
}

// =========================================================================
// Helper: query process creation time
// =========================================================================

uint64_t query_create_time(HANDLE process) {
  KERNEL_USER_TIMES times;
  NTSTATUS st = ::NtQueryInformationProcess(process, ProcessTimes, &times,
                                            sizeof(times), nullptr);
  if (!NT_SUCCESS(st))
    return 0;
  return times.CreateTime.QuadPart;
}

// Deliver one signal by using the ALPC connection request itself as the
// signal payload. This keeps the receiver side listener-only: no accepted
// communication ports need to be tracked for post-connect traffic.
NTSTATUS connect_and_send_signal(pid_t pid, const SignalMessage &request) {
  auto &alpc = g_pcb.signal_alpc;

  HANDLE target_proc = nullptr;
  NTSTATUS st = ::NtOpenProcessById(
      &target_proc, PROCESS_QUERY_LIMITED_INFORMATION,
      static_cast<DWORD>(pid));
  if (!NT_SUCCESS(st))
    return st;

  uint64_t target_ct = query_create_time(target_proc);
  ::NtClose(target_proc);
  if (target_ct == 0)
    return STATUS_OBJECT_NAME_NOT_FOUND;

  WCHAR name_buf[PORT_NAME_BUF_LEN];
  int name_len = format_port_name(name_buf, PORT_NAME_BUF_LEN, pid, target_ct);

  UNICODE_STRING port_name;
  init_unicode_string(&port_name, name_buf, name_len);

  auto oa = windows::named_internal_oa(&port_name, alpc.namespace_handle);

  ALPC_PORT_ATTRIBUTES client_attrs = {};
  client_attrs.Flags = ALPC_PORFLG_NONE;
  client_attrs.SecurityQos.Length = sizeof(SECURITY_QUALITY_OF_SERVICE);
  client_attrs.SecurityQos.ImpersonationLevel = SecurityIdentification;
  client_attrs.SecurityQos.ContextTrackingMode = 0;
  client_attrs.SecurityQos.EffectiveOnly = TRUE;
  client_attrs.MaxMessageLength = sizeof(SignalMessage);
#ifdef _WIN64
  client_attrs.Reserved = 0;
#endif

  LARGE_INTEGER timeout;
  timeout.QuadPart = -5000000LL; // 500ms

  SignalConnectMessage message = {};
  message.request = request;

  SIZE_T connect_len = sizeof(message);
  HANDLE conn_port = nullptr;
  st = ::NtAlpcConnectPortEx(
      &conn_port, &oa, nullptr, &client_attrs, ALPC_MSGFLG_SYNC_REQUEST,
      nullptr, reinterpret_cast<PPORT_MESSAGE>(&message.request), &connect_len,
      nullptr, nullptr, &timeout);

  if (conn_port) {
    ::NtAlpcDisconnectPort(conn_port, 0);
    ::NtClose(conn_port);
  }

  if (!NT_SUCCESS(st))
    return st;

  const SignalReply &reply = message.reply;
  if (connect_len < sizeof(SignalReply) || reply.magic != SIGNAL_REPLY_MAGIC ||
      reply.version != SIGNAL_REPLY_VERSION)
    return STATUS_REPLY_MESSAGE_MISMATCH;

  return reply.status;
}

// =========================================================================
// Helper: format port name into buffer
// =========================================================================
//
// Produces: "NtPosixSig-{pid}-{create_time_hex}"
// Returns the length in WCHARs (excluding NUL).

int format_port_name(WCHAR *buf, int buf_len, pid_t pid,
                     uint64_t create_time) {
  // Copy prefix.
  int pos = 0;
  for (const WCHAR *p = PORT_NAME_PREFIX; *p && pos < buf_len - 1; ++p)
    buf[pos++] = *p;

  // Format PID as decimal.
  WCHAR pid_buf[12];
  int pid_len = 0;
  unsigned int pid_val = static_cast<unsigned int>(pid);
  if (pid_val == 0) {
    pid_buf[pid_len++] = u'0';
  } else {
    while (pid_val > 0 && pid_len < 11) {
      pid_buf[pid_len++] = static_cast<WCHAR>(u'0' + (pid_val % 10));
      pid_val /= 10;
    }
    // Reverse.
    for (int i = 0; i < pid_len / 2; ++i) {
      WCHAR tmp = pid_buf[i];
      pid_buf[i] = pid_buf[pid_len - 1 - i];
      pid_buf[pid_len - 1 - i] = tmp;
    }
  }
  for (int i = 0; i < pid_len && pos < buf_len - 1; ++i)
    buf[pos++] = pid_buf[i];

  // Separator.
  if (pos < buf_len - 1)
    buf[pos++] = u'-';

  // Format create_time as 16-digit hex (zero-padded).
  constexpr WCHAR hex[] = u"0123456789ABCDEF";
  for (int i = 15; i >= 0 && pos < buf_len - 1; --i)
    buf[pos++] = hex[(create_time >> (i * 4)) & 0xF];

  buf[pos] = u'\0';
  return pos;
}

// =========================================================================
// Helper: build UNICODE_STRING from WCHAR buffer
// =========================================================================

void init_unicode_string(UNICODE_STRING *us, WCHAR *buf, int char_len) {
  us->Length = static_cast<USHORT>(char_len * sizeof(WCHAR));
  us->MaximumLength = static_cast<USHORT>((char_len + 1) * sizeof(WCHAR));
  us->Buffer = buf;
}

// =========================================================================
// Helper: query token user SID
// =========================================================================
//
// Returns pointer into the provided buffer. Caller must not free.
// Returns nullptr on failure.

SID *query_token_user_sid(HANDLE token, void *buf, ULONG buf_size) {
  ULONG needed = 0;
  NTSTATUS st = ::NtQueryInformationToken(token, TokenUser, buf, buf_size,
                                          &needed);
  if (!NT_SUCCESS(st))
    return nullptr;
  return reinterpret_cast<TOKEN_USER *>(buf)->User.Sid;
}

// =========================================================================
// Helper: check if SeDebugPrivilege is enabled in current token
// =========================================================================

bool is_debug_privilege_enabled() {
  PRIVILEGE_SET ps;
  ps.PrivilegeCount = 1;
  ps.Control = PRIVILEGE_SET_ALL_NECESSARY;
  ps.Privilege[0].Luid.LowPart = SE_DEBUG_PRIVILEGE;
  ps.Privilege[0].Luid.HighPart = 0;
  ps.Privilege[0].Attributes = 0;

  BOOLEAN result = FALSE;
  NTSTATUS st =
      ::NtPrivilegeCheck(NtCurrentProcessToken(), &ps, &result);
  return NT_SUCCESS(st) && result;
}

// =========================================================================
// Namespace + port creation
// =========================================================================

// Build boundary descriptor with current user SID + medium integrity label.
POBJECT_BOUNDARY_DESCRIPTOR create_boundary_descriptor() {
  UNICODE_STRING ns_name;
  WCHAR ns_buf[] = u"NtPosixSignals";
  init_unicode_string(&ns_name, ns_buf,
                      sizeof(ns_buf) / sizeof(WCHAR) - 1);

  POBJECT_BOUNDARY_DESCRIPTOR boundary =
      ::RtlCreateBoundaryDescriptor(&ns_name, BOUNDARY_DESCRIPTOR_FLAG_NONE);
  if (!boundary)
    return nullptr;

  // Add current user SID.
  alignas(8) UCHAR token_buf[256];
  SID *user_sid =
      query_token_user_sid(NtCurrentProcessToken(), token_buf, sizeof(token_buf));
  if (!user_sid) {
    ::RtlDeleteBoundaryDescriptor(boundary);
    return nullptr;
  }

  NTSTATUS st = ::RtlAddSIDToBoundaryDescriptor(&boundary, user_sid);
  if (!NT_SUCCESS(st)) {
    ::RtlDeleteBoundaryDescriptor(boundary);
    return nullptr;
  }

  // Add medium integrity label: S-1-16-8192 (SECURITY_MANDATORY_MEDIUM_RID).
  SID integrity_sid;
  SID_IDENTIFIER_AUTHORITY label_auth = {
      SECURITY_MANDATORY_LABEL_AUTHORITY_VALUE[0],
      SECURITY_MANDATORY_LABEL_AUTHORITY_VALUE[1],
      SECURITY_MANDATORY_LABEL_AUTHORITY_VALUE[2],
      SECURITY_MANDATORY_LABEL_AUTHORITY_VALUE[3],
      SECURITY_MANDATORY_LABEL_AUTHORITY_VALUE[4],
      SECURITY_MANDATORY_LABEL_AUTHORITY_VALUE[5]};
  ::RtlInitializeSid(&integrity_sid, &label_auth, 1);
  *::RtlSubAuthoritySid(&integrity_sid, 0) = SECURITY_MANDATORY_MEDIUM_RID;

  st = ::RtlAddIntegrityLabelToBoundaryDescriptor(&boundary, &integrity_sid);
  if (!NT_SUCCESS(st)) {
    ::RtlDeleteBoundaryDescriptor(boundary);
    return nullptr;
  }

  return boundary;
}

HANDLE create_or_open_namespace(POBJECT_BOUNDARY_DESCRIPTOR boundary) {
  HANDLE ns = nullptr;
  auto oa = windows::internal_oa();
  NTSTATUS st =
      ::NtCreatePrivateNamespace(&ns, MAXIMUM_ALLOWED, &oa, boundary);
  if (st == STATUS_OBJECT_NAME_COLLISION) {
    // Another same-user process already created it — open instead.
    st = ::NtOpenPrivateNamespace(&ns, MAXIMUM_ALLOWED, &oa, boundary);
  }
  return NT_SUCCESS(st) ? ns : nullptr;
}

// Build port DACL: Creator Owner (GENERIC_WRITE) + Administrators (GENERIC_ALL).
// Returns heap-allocated ACL or nullptr.
ACL *build_port_dacl() {
  // Query current user SID for the Creator Owner ACE.
  alignas(8) UCHAR token_buf[256];
  SID *user_sid =
      query_token_user_sid(NtCurrentProcessToken(), token_buf, sizeof(token_buf));
  if (!user_sid)
    return nullptr;

  ULONG user_sid_len = ::RtlLengthSid(user_sid);

  // Build Administrators SID: S-1-5-32-544.
  SID admin_sid;
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

  // Calculate ACL size.
  // Each ACCESS_ALLOWED_ACE: sizeof(ACE_HEADER) + sizeof(ACCESS_MASK) + SID
  // (minus the SidStart ULONG already in the struct).
  ULONG ace1_size = sizeof(ACE_HEADER) + sizeof(ACCESS_MASK) + user_sid_len;
  ULONG ace2_size = sizeof(ACE_HEADER) + sizeof(ACCESS_MASK) + admin_sid_len;
  ULONG acl_size = sizeof(ACL) + ace1_size + ace2_size;

  // Allocate via page_alloc (no malloc in libc init).
  ACL *dacl = static_cast<ACL *>(page_alloc(acl_size));
  if (!dacl)
    return nullptr;
  dacl->AclRevision = ACL_REVISION;
  dacl->Sbz1 = 0;
  dacl->AclSize = static_cast<USHORT>(acl_size);
  dacl->AceCount = 2;
  dacl->Sbz2 = 0;

  // ACE 1: user SID — same-user processes can connect and send.
  auto *ace1 = reinterpret_cast<ACCESS_ALLOWED_ACE *>(
      reinterpret_cast<UCHAR *>(dacl) + sizeof(ACL));
  ace1->Header.AceType = ACCESS_ALLOWED_ACE_TYPE;
  ace1->Header.AceFlags = 0;
  ace1->Header.AceSize = static_cast<USHORT>(ace1_size);
  ace1->Mask = 0x10000000; // GENERIC_WRITE equivalent for port access.
  ::RtlCopySid(user_sid_len, reinterpret_cast<SID *>(&ace1->SidStart),
               user_sid);

  // ACE 2: Administrators — elevated processes can signal any target.
  auto *ace2 = reinterpret_cast<ACCESS_ALLOWED_ACE *>(
      reinterpret_cast<UCHAR *>(ace1) + ace1_size);
  ace2->Header.AceType = ACCESS_ALLOWED_ACE_TYPE;
  ace2->Header.AceFlags = 0;
  ace2->Header.AceSize = static_cast<USHORT>(ace2_size);
  ace2->Mask = 0x10000000; // GENERIC_ALL equivalent for port access.
  ::RtlCopySid(admin_sid_len, reinterpret_cast<SID *>(&ace2->SidStart),
               &admin_sid);

  return dacl;
}

// Build port security descriptor from DACL.
void build_port_sd(SECURITY_DESCRIPTOR *sd, ACL *dacl) {
  sd->Revision = SECURITY_DESCRIPTOR_REVISION;
  sd->Sbz1 = 0;
  sd->Control = SE_DACL_PRESENT | SE_DACL_PROTECTED;
  sd->Owner = nullptr;
  sd->Group = nullptr;
  sd->Sacl = nullptr;
  sd->Dacl = dacl;
}

// Create the ALPC port inside the private namespace.
HANDLE create_signal_port(HANDLE namespace_handle,
                          SECURITY_DESCRIPTOR *sd,
                          ALPC_PORT_ATTRIBUTES *port_attrs,
                          pid_t pid, uint64_t create_time) {
  WCHAR name_buf[PORT_NAME_BUF_LEN];
  int name_len = format_port_name(name_buf, PORT_NAME_BUF_LEN, pid, create_time);

  UNICODE_STRING port_name;
  init_unicode_string(&port_name, name_buf, name_len);

  auto oa = windows::named_internal_oa(&port_name, namespace_handle, sd);

  // Match the service-port contract used elsewhere in the runtime. The
  // signal transport itself only uses the connection-request handshake.
  port_attrs->Flags = ALPC_PORFLG_ALLOW_LPC_REQUESTS;
  port_attrs->SecurityQos.Length = sizeof(SECURITY_QUALITY_OF_SERVICE);
  port_attrs->SecurityQos.ImpersonationLevel = SecurityIdentification;
  port_attrs->SecurityQos.ContextTrackingMode = 0;
  port_attrs->SecurityQos.EffectiveOnly = TRUE;
  port_attrs->MaxMessageLength = sizeof(SignalMessage);
  port_attrs->MemoryBandwidth = 0;
  port_attrs->MaxPoolUsage = 0;
  port_attrs->MaxSectionSize = 0;
  port_attrs->MaxViewSize = 0;
  port_attrs->MaxTotalSectionSize = 0;
  port_attrs->DupObjectTypes = 0;
#ifdef _WIN64
  port_attrs->Reserved = 0;
#endif

  HANDLE port = nullptr;
  NTSTATUS st = ::NtAlpcCreatePort(&port, &oa, port_attrs);
  return NT_SUCCESS(st) ? port : nullptr;
}

// =========================================================================
// Listener thread
// =========================================================================

// Pend a signal that arrived via ALPC to the process-wide pending set.
void pend_cross_process_signal(int signum, int si_code, pid_t sender_pid,
                               uid_t sender_uid, uint64_t si_value_raw) {
  union sigval sval;
  sval.sival_ptr = reinterpret_cast<void *>(si_value_raw);

  if (signum >= SIGRTMIN && signum <= SIGRTMAX) {
    SigqueueEntry *entry = sigqueue_alloc();
    if (entry) {
      entry->si_signo = signum;
      entry->si_code = si_code;
      entry->value = sval;
      entry->pid = sender_pid;
      entry->uid = sender_uid;
      signal_pending::pend_rt(g_pcb.signal_dispatch.process_pending, entry);
    }
  } else {
    signal_pending::pend_standard(g_pcb.signal_dispatch.process_pending,
                                  signum);
  }
  signal_dispatch::trigger_any_thread();
}

HANDLE open_verified_process_by_pid(pid_t pid, uint64_t expected_create_time) {
  HANDLE process = nullptr;
  NTSTATUS st = ::NtOpenProcessById(
      &process, PROCESS_QUERY_LIMITED_INFORMATION,
      static_cast<DWORD>(pid));
  if (!NT_SUCCESS(st))
    return nullptr;

  if (query_create_time(process) != expected_create_time) {
    ::NtClose(process);
    return nullptr;
  }

  return process;
}

HANDLE open_verified_process_token(pid_t pid, uint64_t expected_create_time) {
  HANDLE process = open_verified_process_by_pid(pid, expected_create_time);
  if (!process)
    return nullptr;

  HANDLE token = nullptr;
  NTSTATUS st = ::NtOpenProcessTokenEx(process, TOKEN_QUERY, 0, &token);
  ::NtClose(process);
  if (!NT_SUCCESS(st))
    return nullptr;
  return token;
}

// Extract sender UID from the sender token after verifying the kernel-attested
// sender PID still refers to the process that originally sent the request.
uid_t extract_sender_uid(pid_t sender_pid, uint64_t sender_create_time) {
  HANDLE sender_token =
      open_verified_process_token(sender_pid, sender_create_time);
  if (!sender_token)
    return static_cast<uid_t>(-1);

  alignas(8) UCHAR token_buf[256];
  SID *sid = query_token_user_sid(sender_token, token_buf, sizeof(token_buf));
  ::NtClose(sender_token);
  if (!sid)
    return static_cast<uid_t>(-1);

  return internal::sid_to_uid(sid);
}

// Check receiver-side permission (defense-in-depth, Tier 3).
// Returns true if the sender is authorized. DACL (Tier 1) already
// filtered unauthorized senders at connect time, so this is a
// secondary check for audit and fine-grained token inspection.
bool check_receiver_permission(pid_t sender_pid, uint64_t sender_create_time) {
  HANDLE sender_token =
      open_verified_process_token(sender_pid, sender_create_time);
  if (!sender_token)
    return false;

  // Get sender's user SID.
  alignas(8) UCHAR sender_buf[256];
  SID *sender_sid = query_token_user_sid(sender_token, sender_buf,
                                         sizeof(sender_buf));
  if (!sender_sid) {
    ::NtClose(sender_token);
    return false;
  }

  // Get our user SID.
  alignas(8) UCHAR our_buf[256];
  SID *our_sid =
      query_token_user_sid(NtCurrentProcessToken(), our_buf, sizeof(our_buf));
  if (!our_sid) {
    ::NtClose(sender_token);
    return false;
  }

  // Same user → allowed.
  if (::RtlEqualSid(sender_sid, our_sid)) {
    ::NtClose(sender_token);
    return true;
  }

  // Check if sender has SeDebugPrivilege (root/CAP_KILL equivalent).
  // We can check this via the sender's token attributes.
  PRIVILEGE_SET ps;
  ps.PrivilegeCount = 1;
  ps.Control = PRIVILEGE_SET_ALL_NECESSARY;
  ps.Privilege[0].Luid.LowPart = SE_DEBUG_PRIVILEGE;
  ps.Privilege[0].Luid.HighPart = 0;
  ps.Privilege[0].Attributes = 0;
  BOOLEAN has_debug = FALSE;
  NTSTATUS st = ::NtPrivilegeCheck(sender_token, &ps, &has_debug);
  ::NtClose(sender_token);
  if (NT_SUCCESS(st) && has_debug)
    return true;

  return false;
}

// Handle a connection request from a client.
void handle_connection_request(HANDLE server_port, PPORT_MESSAGE request,
                               SIZE_T request_length) {
  bool valid_request =
      request_length >= sizeof(SignalMessage) &&
      static_cast<unsigned short>(request->u1.s1.TotalLength) >=
          sizeof(SignalMessage);
  auto *sig_msg = reinterpret_cast<const SignalMessage *>(request);
  if (valid_request) {
    valid_request = sig_msg->magic == SIGNAL_MAGIC &&
                    sig_msg->version == SIGNAL_VERSION &&
                    is_valid_signal(sig_msg->signum) &&
                    sig_msg->sender_create_time != 0;
  }

  pid_t sender_pid = static_cast<pid_t>(
      reinterpret_cast<ULONG_PTR>(request->ClientId.UniqueProcess));
  bool accept = valid_request &&
                check_receiver_permission(sender_pid,
                                          sig_msg->sender_create_time);

  HANDLE comm_port = nullptr;
  ALPC_PORT_ATTRIBUTES accept_attrs = {};
  accept_attrs.Flags = ALPC_PORFLG_ALLOW_LPC_REQUESTS;
  accept_attrs.SecurityQos.Length = sizeof(SECURITY_QUALITY_OF_SERVICE);
  accept_attrs.SecurityQos.ImpersonationLevel = SecurityIdentification;
  accept_attrs.SecurityQos.ContextTrackingMode = 0;
  accept_attrs.SecurityQos.EffectiveOnly = TRUE;
  accept_attrs.MaxMessageLength = sizeof(SignalMessage);
#ifdef _WIN64
  accept_attrs.Reserved = 0;
#endif

  SignalReply reply = {};
  reply.header.u1.s1.DataLength =
      static_cast<CSHORT>(sizeof(SignalReply) - sizeof(PORT_MESSAGE));
  reply.header.u1.s1.TotalLength = static_cast<CSHORT>(sizeof(SignalReply));
  reply.header.MessageId = request->MessageId;
  reply.magic = SIGNAL_REPLY_MAGIC;
  reply.version = SIGNAL_REPLY_VERSION;
  reply.status = accept ? STATUS_SUCCESS : STATUS_ACCESS_DENIED;
  if (!valid_request)
    reply.status = STATUS_INVALID_PARAMETER;

  NTSTATUS status = ::NtAlpcAcceptConnectPort(
      &comm_port, server_port, 0, nullptr, &accept_attrs, nullptr,
      reinterpret_cast<PPORT_MESSAGE>(&reply), nullptr, accept);
  if (NT_SUCCESS(status) && accept && comm_port) {
    uid_t sender_uid =
        extract_sender_uid(sender_pid, sig_msg->sender_create_time);
    pend_cross_process_signal(sig_msg->signum, sig_msg->si_code, sender_pid,
                              sender_uid, sig_msg->value);
  }
  if (comm_port) {
    ::NtAlpcDisconnectPort(comm_port, 0);
    ::NtClose(comm_port);
  }
}

// Reactor callback: invoked by the drain thread when the ALPC port's IOCP
// completion fires. Drains ALL pending messages via non-blocking receive —
// multiple messages may queue between completions.
void alpc_reactor_callback(void * /*context*/, NTSTATUS /*status*/,
                           ULONG_PTR /*information*/) {
  alignas(8) UCHAR recv_buf[RECV_BUF_SIZE];

  // Non-blocking timeout: return immediately if no messages pending.
  LARGE_INTEGER zero_timeout;
  zero_timeout.QuadPart = 0;

  auto &alpc = g_pcb.signal_alpc;

  for (;;) {
    SIZE_T buf_len = RECV_BUF_SIZE;

    NTSTATUS st = ::NtAlpcSendWaitReceivePort(
        alpc.port,
        0,       // Flags (receive only).
        nullptr, // No send message.
        nullptr, // No send attributes.
        reinterpret_cast<PPORT_MESSAGE>(recv_buf), &buf_len, nullptr,
        &zero_timeout); // Non-blocking — drain all pending.

    if (!NT_SUCCESS(st))
      break; // STATUS_TIMEOUT or error — all messages drained.

    auto *msg = reinterpret_cast<PPORT_MESSAGE>(recv_buf);
    CSHORT message_type =
        static_cast<CSHORT>(msg->u2.s2.Type & static_cast<CSHORT>(0x0fff));

    // Connection requests arrive as LPC_CONNECTION_REQUEST.
    if (message_type == LPC_CONNECTION_REQUEST) {
      handle_connection_request(alpc.port, msg, buf_len);
      continue;
    }

    // Signal delivery is carried by the connection request itself.
    // Any non-connection notifications can be ignored here.
  }
}

} // anonymous namespace

// =========================================================================
// Public API
// =========================================================================

namespace alpc_transport {

int init() {
  auto &alpc = g_pcb.signal_alpc;

  // 1. Build boundary descriptor.
  alpc.boundary = create_boundary_descriptor();
  if (!alpc.boundary)
    return -1;

  // 2. Create or open private namespace.
  auto boundary =
      static_cast<POBJECT_BOUNDARY_DESCRIPTOR>(alpc.boundary);
  alpc.namespace_handle = create_or_open_namespace(boundary);
  if (!alpc.namespace_handle) {
    ::RtlDeleteBoundaryDescriptor(boundary);
    alpc.boundary = nullptr;
    return -1;
  }

  // 3. Build port security descriptor (DACL). SD/DACL/attrs are
  //    transport-private (file-local statics) — reused by fork_reinit.
  g_port_dacl = build_port_dacl();
  if (!g_port_dacl) {
    ::NtClose(alpc.namespace_handle);
    alpc.namespace_handle = nullptr;
    ::RtlDeleteBoundaryDescriptor(boundary);
    alpc.boundary = nullptr;
    return -1;
  }
  build_port_sd(&g_port_sd, g_port_dacl);

  // 4. Query our creation time.
  alpc.create_time = query_create_time(NtCurrentProcess());

  // 5. Create the ALPC port.
  alpc.port = create_signal_port(alpc.namespace_handle, &g_port_sd,
                                 &g_port_attrs, get_current_pid(),
                                 alpc.create_time);
  if (!alpc.port) {
    page_free(g_port_dacl);
    g_port_dacl = nullptr;
    ::NtClose(alpc.namespace_handle);
    alpc.namespace_handle = nullptr;
    ::RtlDeleteBoundaryDescriptor(boundary);
    alpc.boundary = nullptr;
    return -1;
  }

  // 6. Register with process-wide reactor. The reactor's IOCP receives a
  //    completion each time a message arrives on the ALPC port, and the
  //    drain thread calls alpc_reactor_callback to process it.
  g_reactor_token = reactor::watch_alpc(alpc.port, alpc_reactor_callback,
                                        nullptr);
  if (!g_reactor_token.valid()) {
    ::NtClose(alpc.port);
    alpc.port = nullptr;
    page_free(g_port_dacl);
    g_port_dacl = nullptr;
    ::NtClose(alpc.namespace_handle);
    alpc.namespace_handle = nullptr;
    ::RtlDeleteBoundaryDescriptor(boundary);
    alpc.boundary = nullptr;
    return -1;
  }

  return 0;
}

void fini() {
  auto &alpc = g_pcb.signal_alpc;

  // Deregister from reactor. After this returns, no callback is running
  // and none will fire — safe to close the port.
  if (g_reactor_token.valid()) {
    reactor::unwatch(g_reactor_token);
    g_reactor_token = reactor::INVALID_TOKEN;
  }

  if (alpc.port) {
    ::NtClose(alpc.port);
    alpc.port = nullptr;
  }

  // Close namespace handle.
  if (alpc.namespace_handle) {
    ::NtClose(alpc.namespace_handle);
    alpc.namespace_handle = nullptr;
  }

  // Free DACL.
  if (g_port_dacl) {
    page_free(g_port_dacl);
    g_port_dacl = nullptr;
  }

  // Free boundary descriptor.
  if (alpc.boundary) {
    ::RtlDeleteBoundaryDescriptor(
        static_cast<POBJECT_BOUNDARY_DESCRIPTOR>(alpc.boundary));
    alpc.boundary = nullptr;
  }
}

void fork_reinit() {
  auto &alpc = g_pcb.signal_alpc;

  // 1. The parent's ALPC port was created with named_internal_oa()
  //    (non-inheritable), so it doesn't exist in the child's handle table.
  //    Just null the stale pointer — no NtClose.
  alpc.port = nullptr;

  // 2. Parent's reactor token is invalid — reactor::fork_reinit() already
  //    invalidated all registrations and created a new IOCP.
  g_reactor_token = reactor::INVALID_TOKEN;

  // 3. The private namespace handle is still valid (same user, same
  //    boundary descriptor). Reuse it.

  // 4. Query the CHILD's creation time (different from parent's).
  alpc.create_time = query_create_time(NtCurrentProcess());

  // 5. Create the child's own ALPC port with the child's PID.
  alpc.port = create_signal_port(alpc.namespace_handle, &g_port_sd,
                                 &g_port_attrs, get_current_pid(),
                                 alpc.create_time);

  // 6. Register the child's port with the reactor (which was already
  //    reinitialized by reactor_fork_reinit(), before signal_fork_reinit()).
  if (alpc.port) {
    g_reactor_token = reactor::watch_alpc(alpc.port, alpc_reactor_callback,
                                          nullptr);
  }
}

void pre_exec_cleanup() {
  auto &alpc = g_pcb.signal_alpc;

  // Deregister from reactor before exec. The exec'd image's CRT init
  // will create its own port and register with its own reactor.
  if (g_reactor_token.valid()) {
    reactor::unwatch(g_reactor_token);
    g_reactor_token = reactor::INVALID_TOKEN;
  }

  // Close the port before exec. The exec'd image's CRT init will
  // create a new port with the same name (same PID + create_time).
  if (alpc.port) {
    ::NtClose(alpc.port);
    alpc.port = nullptr;
  }

}

intptr_t send_to_process(pid_t pid, int signum, const siginfo_t *info) {
  if (!is_valid_signal(signum))
    return -EINVAL;

  // Signal 0: existence check only.
  if (signum == 0) {
    HANDLE h = nullptr;
    NTSTATUS st = ::NtOpenProcessById(
        &h, PROCESS_QUERY_LIMITED_INFORMATION, static_cast<DWORD>(pid));
    if (!NT_SUCCESS(st))
      return -ESRCH;
    ::NtClose(h);
    return 0;
  }

  // ALPC not initialized — fall back to ESRCH (cross-process disabled).
  if (!g_pcb.signal_alpc.port)
    return -ESRCH;

  // Sender-side permission check (Tier 2 — for synchronous EPERM).
  int perm = check_send_permission(pid);
  if (perm != 0)
    return perm;

  // Build signal message.
  SignalMessage msg = {};
  msg.header.u1.s1.DataLength =
      static_cast<CSHORT>(sizeof(SignalMessage) - sizeof(PORT_MESSAGE));
  msg.header.u1.s1.TotalLength = static_cast<CSHORT>(sizeof(SignalMessage));
  msg.magic = SIGNAL_MAGIC;
  msg.version = SIGNAL_VERSION;
  msg.signum = static_cast<uint8_t>(signum);
  msg.si_code = info ? info->si_code : SI_USER;
  msg.sender_create_time = g_pcb.signal_alpc.create_time;
  msg.value = info ? reinterpret_cast<uint64_t>(info->si_value.sival_ptr) : 0;

  NTSTATUS st = connect_and_send_signal(pid, msg);
  return NT_SUCCESS(st) ? 0 : -ESRCH;
}

int check_send_permission(pid_t target_pid) {
  // Open target process for token query.
  HANDLE target_proc = nullptr;
  NTSTATUS st = ::NtOpenProcessById(
      &target_proc, PROCESS_QUERY_LIMITED_INFORMATION,
      static_cast<DWORD>(target_pid));
  if (!NT_SUCCESS(st))
    return -ESRCH;

  // Get target's process token user SID (real_uid equivalent).
  HANDLE target_token = nullptr;
  st = ::NtOpenProcessTokenEx(target_proc, TOKEN_QUERY, 0, &target_token);
  ::NtClose(target_proc);
  if (!NT_SUCCESS(st))
    return -EPERM;

  alignas(8) UCHAR target_buf[256];
  SID *target_sid =
      query_token_user_sid(target_token, target_buf, sizeof(target_buf));
  ::NtClose(target_token);
  if (!target_sid)
    return -EPERM;

  // Get our process token user SID (real_uid).
  alignas(8) UCHAR our_buf[256];
  SID *our_sid =
      query_token_user_sid(NtCurrentProcessToken(), our_buf, sizeof(our_buf));
  if (!our_sid)
    return -EPERM;

  // POSIX: sender.ruid == target.ruid
  if (::RtlEqualSid(our_sid, target_sid))
    return 0;

  // Check thread impersonation token (effective_uid).
  HANDLE thread_token = nullptr;
  st = ::NtOpenThreadTokenEx(NtCurrentThread(), TOKEN_QUERY, TRUE, 0,
                             &thread_token);
  if (NT_SUCCESS(st)) {
    alignas(8) UCHAR eff_buf[256];
    SID *eff_sid =
        query_token_user_sid(thread_token, eff_buf, sizeof(eff_buf));
    ::NtClose(thread_token);
    if (eff_sid && ::RtlEqualSid(eff_sid, target_sid))
      return 0;
  }

  // CAP_KILL equivalent: SeDebugPrivilege.
  if (is_debug_privilege_enabled())
    return 0;

  return -EPERM;
}

} // namespace alpc_transport
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL
