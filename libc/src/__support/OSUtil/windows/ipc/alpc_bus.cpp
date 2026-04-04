//===-- Generic ALPC bus — multiplexed cross-process IPC -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// See alpc_bus.h for the architectural overview and security model. The
// runtime shape of this file mirrors the original signal_state::alpc_transport
// it grew out of: private-namespace boundary, port DACL (creator-owner +
// administrators), reactor-driven receive, and one-shot connection-request
// delivery with the reply carried on the same connection handshake.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/ipc/alpc_bus.h"

#include "src/__support/OSUtil/windows/ipc/alpc_bus_state.h"
#include "src/__support/OSUtil/windows/reactor/reactor.h"

#include "src/__support/CPP/atomic.h"
#include "src/__support/CPP/span.h"
#include "src/__support/OSUtil/windows/alloc/page_alloc.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/nt_ipc.h"
#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/OSUtil/windows/nt/nt_security.h"
#include "src/__support/OSUtil/windows/nt/nt_wstringstream.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/process/sid_utils.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace alpc_bus {

namespace reactor = internal::reactor;

namespace {

// =========================================================================
// Constants
// =========================================================================

// Private-namespace name. All NT-POSIX processes of the same user share
// this namespace; port names inside are per-process (PID + create_time).
constexpr WCHAR NAMESPACE_NAME[] = u"NtPosixBus";

// Port name prefix. Keeping the legacy "NtPosixSig-" token would have
// baked the original subsystem into a name that now serves every consumer —
// fresh prefix keeps the directory honest.
constexpr WCHAR PORT_NAME_PREFIX[] = u"NtPosixBus-";

// Max port-name length: prefix + PID (10 digits) + '-' + create_time
// (16 hex) + NUL.
constexpr int PORT_NAME_BUF_LEN = 64;

// Maximum inbound frame size: port header + bus header + max payload.
// The receive buffer is sized larger than strictly needed so truncation
// errors from a mis-specified sender manifest as "too big" → rejected
// instead of silent corruption.
constexpr SIZE_T RECV_BUF_SIZE = sizeof(BusFrame) + 64;

// SeDebugPrivilege LUID — root / CAP_KILL equivalent. Exposed so consumers
// that inherited this concept (signal delivery, future process inspection)
// do not need to duplicate the constant.
constexpr ULONG SE_DEBUG_PRIVILEGE = 20;

// Magic word for the reply header. Different from the request magic so a
// misrouted reply-as-request can't be mistaken for a valid request.
constexpr uint32_t BUS_REPLY_MAGIC = 0x52534542;   // 'BESR' in LE
constexpr uint16_t BUS_REPLY_VERSION = 1;

// =========================================================================
// Handler dispatch table
// =========================================================================
//
// Handlers are registered at subsystem init (before `init()` typically)
// and never change after that — storage is a fixed array indexed by
// opcode. No locking needed on the read path; registration uses atomic
// CAS so duplicate registrations are rejected deterministically.

constexpr uint16_t MAX_OPCODES = 64;

cpp::Atomic<Handler> g_handlers[MAX_OPCODES] = {};

LIBC_INLINE Handler load_handler(uint16_t op) {
  if (op == OP_INVALID || op >= MAX_OPCODES)
    return nullptr;
  return g_handlers[op].load(cpp::MemoryOrder::ACQUIRE);
}

// =========================================================================
// Transport-private state
// =========================================================================
//
// These fields are implementation details of the bus lifecycle and are
// not exposed to other subsystems. The observable process-wide bus state
// lives in g_pcb.alpc_bus.

SECURITY_DESCRIPTOR      g_port_sd = {};
ACL                     *g_port_dacl = nullptr;
ALPC_PORT_ATTRIBUTES     g_port_attrs = {};
reactor::ReactorToken    g_reactor_token = reactor::INVALID_TOKEN;

// =========================================================================
// Wire-format validation
// =========================================================================

static_assert(sizeof(pid_t) == sizeof(int32_t),
              "pid_t must be 32-bit for bus naming");
static_assert(sizeof(BusFrame) ==
                  sizeof(PORT_MESSAGE) + sizeof(BusHeader) + BUS_MAX_PAYLOAD,
              "BusFrame must have no padding between its three parts");

// =========================================================================
// Small helpers shared by multiple paths
// =========================================================================

LIBC_INLINE uint64_t query_create_time(HANDLE process) {
  KERNEL_USER_TIMES times;
  NTSTATUS st = ::NtQueryInformationProcess(process, ProcessTimes, &times,
                                            sizeof(times), nullptr);
  if (!NT_SUCCESS(st))
    return 0;
  return times.CreateTime.QuadPart;
}

// Produces: "NtPosixBus-{pid}-{create_time_hex}". Returns the length in
// WCHARs, not including the NUL terminator.
int format_port_name(WCHAR *buf, int buf_len, pid_t pid,
                     uint64_t create_time) {
  using windows::WStringStream;
  WStringStream ss(cpp::span<WCHAR>(buf, static_cast<size_t>(buf_len - 1)));
  ss << PORT_NAME_PREFIX
     << static_cast<unsigned int>(pid)
     << u'-';
  ss.write_int<radix::Hex::Uppercase::WithWidth<16>>(create_time);
  ss.null_terminate();
  return static_cast<int>(ss.str().size());
}

// Returns a pointer INTO `buf` (no ownership). Caller-supplied buffer
// must outlive the returned pointer.
SID *query_token_user_sid(HANDLE token, void *buf, ULONG buf_size) {
  ULONG needed = 0;
  NTSTATUS st = ::NtQueryInformationToken(token, TokenUser, buf, buf_size,
                                          &needed);
  if (!NT_SUCCESS(st))
    return nullptr;
  return reinterpret_cast<TOKEN_USER *>(buf)->User.Sid;
}

bool token_has_debug_privilege(HANDLE token) {
  PRIVILEGE_SET ps;
  ps.PrivilegeCount = 1;
  ps.Control = PRIVILEGE_SET_ALL_NECESSARY;
  ps.Privilege[0].Luid.LowPart = SE_DEBUG_PRIVILEGE;
  ps.Privilege[0].Luid.HighPart = 0;
  ps.Privilege[0].Attributes = 0;
  BOOLEAN result = FALSE;
  NTSTATUS st = ::NtPrivilegeCheck(token, &ps, &result);
  return NT_SUCCESS(st) && result;
}

// =========================================================================
// Boundary descriptor / private namespace / port DACL
// =========================================================================

POBJECT_BOUNDARY_DESCRIPTOR create_boundary_descriptor() {
  windows::nt_wstring_view ns_name(NAMESPACE_NAME);

  POBJECT_BOUNDARY_DESCRIPTOR boundary = ::RtlCreateBoundaryDescriptor(
      ns_name.unicode_string(), BOUNDARY_DESCRIPTOR_FLAG_NONE);
  if (!boundary)
    return nullptr;

  // Scope the namespace to the current user's SID — other users cannot
  // even enumerate the namespace, let alone connect to its ports.
  alignas(8) UCHAR token_buf[256];
  SID *user_sid = query_token_user_sid(NtCurrentProcessToken(), token_buf,
                                       sizeof(token_buf));
  if (!user_sid) {
    ::RtlDeleteBoundaryDescriptor(boundary);
    return nullptr;
  }
  if (!NT_SUCCESS(::RtlAddSIDToBoundaryDescriptor(&boundary, user_sid))) {
    ::RtlDeleteBoundaryDescriptor(boundary);
    return nullptr;
  }

  // Add medium integrity label — lower-IL (sandbox) processes cannot
  // open the namespace even within the same user.
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

  if (!NT_SUCCESS(
          ::RtlAddIntegrityLabelToBoundaryDescriptor(&boundary, &integrity_sid))) {
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
    // A peer process of the same user already created it — open instead.
    st = ::NtOpenPrivateNamespace(&ns, MAXIMUM_ALLOWED, &oa, boundary);
  }
  return NT_SUCCESS(st) ? ns : nullptr;
}

// Two-ACE DACL: Creator Owner (GENERIC_WRITE — connect/send) and
// Administrators (GENERIC_ALL — elevated processes can reach any peer).
ACL *build_port_dacl() {
  alignas(8) UCHAR token_buf[256];
  SID *user_sid = query_token_user_sid(NtCurrentProcessToken(), token_buf,
                                       sizeof(token_buf));
  if (!user_sid)
    return nullptr;
  ULONG user_sid_len = ::RtlLengthSid(user_sid);

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

  ULONG ace1_size = sizeof(ACE_HEADER) + sizeof(ACCESS_MASK) + user_sid_len;
  ULONG ace2_size = sizeof(ACE_HEADER) + sizeof(ACCESS_MASK) + admin_sid_len;
  ULONG acl_size = sizeof(ACL) + ace1_size + ace2_size;

  auto *dacl = static_cast<ACL *>(page_alloc(acl_size));
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
  ace1->Mask = 0x10000000; // GENERIC_WRITE — connect + send.
  ::RtlCopySid(user_sid_len, reinterpret_cast<SID *>(&ace1->SidStart),
               user_sid);

  auto *ace2 = reinterpret_cast<ACCESS_ALLOWED_ACE *>(
      reinterpret_cast<UCHAR *>(ace1) + ace1_size);
  ace2->Header.AceType = ACCESS_ALLOWED_ACE_TYPE;
  ace2->Header.AceFlags = 0;
  ace2->Header.AceSize = static_cast<USHORT>(ace2_size);
  ace2->Mask = 0x10000000; // GENERIC_ALL — administrators.
  ::RtlCopySid(admin_sid_len, reinterpret_cast<SID *>(&ace2->SidStart),
               &admin_sid);

  return dacl;
}

void build_port_sd(SECURITY_DESCRIPTOR *sd, ACL *dacl) {
  sd->Revision = SECURITY_DESCRIPTOR_REVISION;
  sd->Sbz1 = 0;
  sd->Control = SE_DACL_PRESENT | SE_DACL_PROTECTED;
  sd->Owner = nullptr;
  sd->Group = nullptr;
  sd->Sacl = nullptr;
  sd->Dacl = dacl;
}

HANDLE create_bus_port(HANDLE namespace_handle, SECURITY_DESCRIPTOR *sd,
                       ALPC_PORT_ATTRIBUTES *port_attrs, pid_t pid,
                       uint64_t create_time) {
  WCHAR name_buf[PORT_NAME_BUF_LEN];
  int name_len =
      format_port_name(name_buf, PORT_NAME_BUF_LEN, pid, create_time);

  windows::nt_wstring_view port_name(name_buf, name_len);
  auto oa = windows::named_internal_oa(&port_name, namespace_handle, sd);

  port_attrs->Flags = ALPC_PORFLG_ALLOW_LPC_REQUESTS;
  port_attrs->SecurityQos.Length = sizeof(SECURITY_QUALITY_OF_SERVICE);
  port_attrs->SecurityQos.ImpersonationLevel = SecurityIdentification;
  port_attrs->SecurityQos.ContextTrackingMode = 0;
  port_attrs->SecurityQos.EffectiveOnly = TRUE;
  port_attrs->MaxMessageLength = sizeof(BusFrame);
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
// Sender-identity verification (receiver side)
// =========================================================================

HANDLE open_verified_process_by_pid(pid_t pid, uint64_t expected_create_time) {
  windows::ScopedNtHandle process;
  NTSTATUS st = ::NtOpenProcessById(process.put(),
                                    PROCESS_QUERY_LIMITED_INFORMATION,
                                    static_cast<DWORD>(pid));
  if (!NT_SUCCESS(st))
    return nullptr;

  if (query_create_time(process.get()) != expected_create_time)
    return nullptr;

  return process.release();
}

HANDLE open_verified_process_token(pid_t pid, uint64_t expected_create_time) {
  windows::ScopedNtHandle process(
      open_verified_process_by_pid(pid, expected_create_time));
  if (!process)
    return nullptr;

  HANDLE token = nullptr;
  NTSTATUS st = ::NtOpenProcessTokenEx(process.get(), TOKEN_QUERY, 0, &token);
  if (!NT_SUCCESS(st))
    return nullptr;
  return token;
}

// Resolves `sender_pid`/`sender_create_time` to a POSIX UID, with PID-reuse
// revalidation baked in: if the process exited and its PID got recycled,
// the create_time won't match and we return -1.
uid_t resolve_sender_uid(pid_t sender_pid, uint64_t sender_create_time) {
  windows::ScopedNtHandle sender_token(
      open_verified_process_token(sender_pid, sender_create_time));
  if (!sender_token)
    return static_cast<uid_t>(-1);

  alignas(8) UCHAR token_buf[256];
  SID *sid =
      query_token_user_sid(sender_token.get(), token_buf, sizeof(token_buf));
  if (!sid)
    return static_cast<uid_t>(-1);

  return sid_to_uid(sid);
}

// =========================================================================
// Receive / dispatch
// =========================================================================

struct DispatchResult {
  NTSTATUS status;       // echoed back to the sender in BusHeader.flags_or_status
  uint32_t reply_len;    // valid only if status == STATUS_SUCCESS
};

// Run the handler for a validated request. Returns the NTSTATUS to embed
// in the reply and the number of reply payload bytes written. All caller
// buffers must be sized for BUS_MAX_PAYLOAD.
DispatchResult dispatch_request(const BusHeader &req_hdr,
                                const void *request_payload,
                                pid_t sender_pid, void *reply_payload) {
  // Defense in depth: the header was already framing-validated by the
  // receive path, but recheck fields that a clever sender could have
  // crafted to slip past the framing checks (e.g. a valid total length
  // with an oversized payload_len).
  if (req_hdr.payload_len > BUS_MAX_PAYLOAD)
    return {STATUS_INVALID_PARAMETER, 0};

  Handler h = load_handler(req_hdr.opcode);
  if (h == nullptr)
    return {STATUS_NOT_SUPPORTED, 0};

  SenderInfo sender{};
  sender.pid = sender_pid;
  sender.create_time = req_hdr.sender_create_time;
  sender.uid = resolve_sender_uid(sender_pid, req_hdr.sender_create_time);

  // If UID resolution failed, the sender is either dead (PID reused
  // between the connect and now) or we don't have PROCESS_QUERY rights.
  // Either way, refuse — no handler should see an unattested peer.
  if (sender.uid == static_cast<uid_t>(-1))
    return {STATUS_ACCESS_DENIED, 0};

  uint32_t reply_len = 0;
  RequestView req{request_payload, req_hdr.payload_len};
  ReplyBuffer rep{reply_payload, BUS_MAX_PAYLOAD, &reply_len};

  int32_t handler_status = h(req, sender, rep);
  if (handler_status != STATUS_SUCCESS)
    return {handler_status, 0};

  if (reply_len > BUS_MAX_PAYLOAD)
    return {STATUS_BUFFER_OVERFLOW, 0};

  return {STATUS_SUCCESS, reply_len};
}

// Build the reply frame that the bus sends back via
// NtAlpcAcceptConnectPort. `reply_frame` must be sized for BusFrame.
void build_reply_frame(BusFrame &reply_frame, const BusHeader &req_hdr,
                       DispatchResult res, ULONG request_msg_id,
                       uint32_t reply_payload_bytes) {
  // PORT_MESSAGE header — only TotalLength and MessageId carry meaning
  // on the reply side; the rest is filled in by the kernel.
  reply_frame.port_header = {};
  CSHORT total =
      static_cast<CSHORT>(sizeof(PORT_MESSAGE) + sizeof(BusHeader) +
                          reply_payload_bytes);
  reply_frame.port_header.u1.s1.DataLength =
      static_cast<CSHORT>(total - sizeof(PORT_MESSAGE));
  reply_frame.port_header.u1.s1.TotalLength = total;
  reply_frame.port_header.MessageId = request_msg_id;

  reply_frame.bus_header.magic = BUS_REPLY_MAGIC;
  reply_frame.bus_header.version = BUS_REPLY_VERSION;
  reply_frame.bus_header.opcode = req_hdr.opcode;
  reply_frame.bus_header.payload_len = reply_payload_bytes;
  reply_frame.bus_header.flags_or_status = res.status;
  reply_frame.bus_header.sender_create_time =
      g_pcb.alpc_bus.create_time;
}

void handle_connection_request(HANDLE server_port, PPORT_MESSAGE request,
                               SIZE_T request_length) {
  // Framing validation: total length must cover the bus header, and the
  // PORT_MESSAGE.TotalLength must agree with the receive-path length.
  bool framing_ok =
      request_length >= sizeof(PORT_MESSAGE) + sizeof(BusHeader) &&
      static_cast<unsigned short>(request->u1.s1.TotalLength) >=
          sizeof(PORT_MESSAGE) + sizeof(BusHeader);

  auto *req_hdr = reinterpret_cast<const BusHeader *>(
      reinterpret_cast<const UCHAR *>(request) + sizeof(PORT_MESSAGE));
  const UCHAR *req_payload =
      reinterpret_cast<const UCHAR *>(request) + sizeof(PORT_MESSAGE) +
      sizeof(BusHeader);

  if (framing_ok) {
    framing_ok =
        req_hdr->magic == BUS_MAGIC && req_hdr->version == BUS_VERSION &&
        req_hdr->sender_create_time != 0 &&
        req_hdr->payload_len <= BUS_MAX_PAYLOAD &&
        request_length >=
            sizeof(PORT_MESSAGE) + sizeof(BusHeader) + req_hdr->payload_len;
  }

  pid_t sender_pid = static_cast<pid_t>(
      reinterpret_cast<ULONG_PTR>(request->ClientId.UniqueProcess));

  // Run the handler (or synthesize a rejection if framing failed). The
  // reply frame is always sent so the peer's NtAlpcConnectPortEx returns
  // promptly with a status instead of timing out.
  BusFrame reply_frame{};
  DispatchResult res{};
  if (!framing_ok) {
    res = {STATUS_INVALID_PARAMETER, 0};
  } else {
    res = dispatch_request(*req_hdr, req_payload, sender_pid,
                           reply_frame.payload);
  }
  build_reply_frame(reply_frame, *req_hdr, res, request->MessageId,
                    res.reply_len);

  ALPC_PORT_ATTRIBUTES accept_attrs = {};
  accept_attrs.Flags = ALPC_PORFLG_ALLOW_LPC_REQUESTS;
  accept_attrs.SecurityQos.Length = sizeof(SECURITY_QUALITY_OF_SERVICE);
  accept_attrs.SecurityQos.ImpersonationLevel = SecurityIdentification;
  accept_attrs.SecurityQos.ContextTrackingMode = 0;
  accept_attrs.SecurityQos.EffectiveOnly = TRUE;
  accept_attrs.MaxMessageLength = sizeof(BusFrame);
#ifdef _WIN64
  accept_attrs.Reserved = 0;
#endif

  windows::ScopedNtHandle comm_port;
  BOOLEAN accept = (res.status == STATUS_SUCCESS) ||
                   (res.status != STATUS_INVALID_PARAMETER);
  // We accept the connection in all non-framing-error cases so the reply
  // frame is actually delivered. The handler's NTSTATUS tells the peer
  // whether the semantic operation succeeded — the connection itself
  // always terminates immediately after the reply.
  (void)::NtAlpcAcceptConnectPort(
      comm_port.put(), server_port, 0, nullptr, &accept_attrs, nullptr,
      reinterpret_cast<PPORT_MESSAGE>(&reply_frame), nullptr, accept);
  if (comm_port)
    ::NtAlpcDisconnectPort(comm_port.get(), 0);
}

// Reactor callback: fires when the port's IOCP completion arrives. Drains
// every pending connection request in a tight loop — ALPC may coalesce
// multiple arrivals into a single completion.
void reactor_callback(void * /*context*/, NTSTATUS /*status*/,
                      ULONG_PTR /*information*/) {
  alignas(8) UCHAR recv_buf[RECV_BUF_SIZE];
  LARGE_INTEGER zero_timeout;
  zero_timeout.QuadPart = 0;

  HANDLE port = g_pcb.alpc_bus.port;
  if (!port)
    return;

  for (;;) {
    SIZE_T buf_len = RECV_BUF_SIZE;

    NTSTATUS st = ::NtAlpcSendWaitReceivePort(
        port,
        0,       // receive only
        nullptr, // no send
        nullptr, // no send attributes
        reinterpret_cast<PPORT_MESSAGE>(recv_buf), &buf_len, nullptr,
        &zero_timeout);

    if (!NT_SUCCESS(st))
      break; // STATUS_TIMEOUT (queue drained) or error.

    auto *msg = reinterpret_cast<PPORT_MESSAGE>(recv_buf);
    CSHORT message_type =
        static_cast<CSHORT>(msg->u2.s2.Type & static_cast<CSHORT>(0x0fff));

    // Bus requests arrive as LPC_CONNECTION_REQUEST — the connection
    // handshake carries both the request and the reply.
    if (message_type == LPC_CONNECTION_REQUEST)
      handle_connection_request(port, msg, buf_len);

    // Any other message type is benign (tear-down notifications, etc.).
  }
}

} // anonymous namespace

// =========================================================================
// Public API — registration and request
// =========================================================================

int register_handler(uint16_t op, Handler handler) {
  if (op == OP_INVALID || op >= MAX_OPCODES || handler == nullptr)
    return -1;
  Handler expected = nullptr;
  if (!g_handlers[op].compare_exchange_strong(expected, handler,
                                              cpp::MemoryOrder::RELEASE,
                                              cpp::MemoryOrder::RELAXED))
    return -1; // already registered — double registration is a bug.
  return 0;
}

int32_t request(pid_t target_pid, uint16_t opcode, const void *payload,
                uint32_t payload_len, void *reply_buf, uint32_t reply_cap,
                uint32_t *reply_len, int64_t timeout_100ns) {
  if (opcode == OP_INVALID || opcode >= MAX_OPCODES)
    return STATUS_INVALID_PARAMETER;
  if (payload_len > BUS_MAX_PAYLOAD)
    return STATUS_INVALID_PARAMETER;
  if (payload_len > 0 && payload == nullptr)
    return STATUS_INVALID_PARAMETER;

  AlpcBusState &bus = g_pcb.alpc_bus;
  if (!bus.namespace_handle)
    return STATUS_PORT_DISCONNECTED; // bus never brought up.

  // Resolve target — validates the target is actually a running process
  // and captures its create_time for port-name formation.
  windows::ScopedNtHandle target_proc;
  NTSTATUS st = ::NtOpenProcessById(target_proc.put(),
                                    PROCESS_QUERY_LIMITED_INFORMATION,
                                    static_cast<DWORD>(target_pid));
  if (!NT_SUCCESS(st))
    return st;

  uint64_t target_ct = query_create_time(target_proc.get());
  if (target_ct == 0)
    return STATUS_OBJECT_NAME_NOT_FOUND;

  WCHAR name_buf[PORT_NAME_BUF_LEN];
  int name_len =
      format_port_name(name_buf, PORT_NAME_BUF_LEN, target_pid, target_ct);
  windows::nt_wstring_view port_name(name_buf, name_len);
  auto oa = windows::named_internal_oa(&port_name, bus.namespace_handle);

  ALPC_PORT_ATTRIBUTES client_attrs = {};
  client_attrs.Flags = ALPC_PORFLG_NONE;
  client_attrs.SecurityQos.Length = sizeof(SECURITY_QUALITY_OF_SERVICE);
  client_attrs.SecurityQos.ImpersonationLevel = SecurityIdentification;
  client_attrs.SecurityQos.ContextTrackingMode = 0;
  client_attrs.SecurityQos.EffectiveOnly = TRUE;
  client_attrs.MaxMessageLength = sizeof(BusFrame);
#ifdef _WIN64
  client_attrs.Reserved = 0;
#endif

  // Build the outbound frame. The connection handshake carries the
  // request; the reply comes back in the same buffer after the kernel
  // fills the PORT_MESSAGE header on completion.
  BusFrame frame{};
  CSHORT total =
      static_cast<CSHORT>(sizeof(PORT_MESSAGE) + sizeof(BusHeader) + payload_len);
  frame.port_header.u1.s1.DataLength =
      static_cast<CSHORT>(total - sizeof(PORT_MESSAGE));
  frame.port_header.u1.s1.TotalLength = total;
  frame.bus_header.magic = BUS_MAGIC;
  frame.bus_header.version = BUS_VERSION;
  frame.bus_header.opcode = opcode;
  frame.bus_header.payload_len = payload_len;
  frame.bus_header.flags_or_status = 0;
  frame.bus_header.sender_create_time = bus.create_time;
  if (payload_len > 0) {
    UCHAR *dst = frame.payload;
    const UCHAR *src = static_cast<const UCHAR *>(payload);
    for (uint32_t i = 0; i < payload_len; ++i)
      dst[i] = src[i];
  }

  LARGE_INTEGER timeout;
  timeout.QuadPart = timeout_100ns;

  SIZE_T connect_len = sizeof(frame);
  windows::ScopedNtHandle conn_port;
  st = ::NtAlpcConnectPortEx(
      conn_port.put(), &oa, nullptr, &client_attrs, ALPC_MSGFLG_SYNC_REQUEST,
      nullptr, reinterpret_cast<PPORT_MESSAGE>(&frame), &connect_len, nullptr,
      nullptr, &timeout);

  if (conn_port)
    ::NtAlpcDisconnectPort(conn_port.get(), 0);

  if (!NT_SUCCESS(st))
    return st;

  // Validate the reply frame. A confused peer (wrong magic/version/opcode)
  // is a hard failure — we treat it as STATUS_REPLY_MESSAGE_MISMATCH rather
  // than propagating possibly-malicious bytes to the caller.
  if (connect_len < sizeof(PORT_MESSAGE) + sizeof(BusHeader))
    return STATUS_REPLY_MESSAGE_MISMATCH;

  const BusHeader &reply_hdr = frame.bus_header;
  if (reply_hdr.magic != BUS_REPLY_MAGIC ||
      reply_hdr.version != BUS_REPLY_VERSION ||
      reply_hdr.opcode != opcode ||
      reply_hdr.payload_len > BUS_MAX_PAYLOAD)
    return STATUS_REPLY_MESSAGE_MISMATCH;

  if (reply_hdr.flags_or_status != STATUS_SUCCESS) {
    // Handler-reported failure. Do not copy reply bytes.
    if (reply_len)
      *reply_len = 0;
    return reply_hdr.flags_or_status;
  }

  if (reply_buf != nullptr && reply_cap > 0) {
    uint32_t copy = reply_hdr.payload_len < reply_cap ? reply_hdr.payload_len
                                                      : reply_cap;
    UCHAR *dst = static_cast<UCHAR *>(reply_buf);
    for (uint32_t i = 0; i < copy; ++i)
      dst[i] = frame.payload[i];
    if (reply_len)
      *reply_len = copy;
  } else if (reply_len) {
    *reply_len = 0;
  }

  return STATUS_SUCCESS;
}

// =========================================================================
// Public API — lifecycle
// =========================================================================

int init() {
  AlpcBusState &bus = g_pcb.alpc_bus;

  bus.boundary = create_boundary_descriptor();
  if (!bus.boundary)
    return -1;

  auto boundary = static_cast<POBJECT_BOUNDARY_DESCRIPTOR>(bus.boundary);
  bus.namespace_handle = create_or_open_namespace(boundary);
  if (!bus.namespace_handle) {
    ::RtlDeleteBoundaryDescriptor(boundary);
    bus.boundary = nullptr;
    return -1;
  }

  g_port_dacl = build_port_dacl();
  if (!g_port_dacl) {
    ::NtClose(bus.namespace_handle);
    bus.namespace_handle = nullptr;
    ::RtlDeleteBoundaryDescriptor(boundary);
    bus.boundary = nullptr;
    return -1;
  }
  build_port_sd(&g_port_sd, g_port_dacl);

  bus.create_time = query_create_time(NtCurrentProcess());

  bus.port = create_bus_port(bus.namespace_handle, &g_port_sd, &g_port_attrs,
                             static_cast<pid_t>(NtCurrentProcessId()),
                             bus.create_time);
  if (!bus.port) {
    page_free(g_port_dacl);
    g_port_dacl = nullptr;
    ::NtClose(bus.namespace_handle);
    bus.namespace_handle = nullptr;
    ::RtlDeleteBoundaryDescriptor(boundary);
    bus.boundary = nullptr;
    return -1;
  }

  g_reactor_token = reactor::watch_alpc(bus.port, reactor_callback, nullptr);
  if (!g_reactor_token.valid()) {
    ::NtClose(bus.port);
    bus.port = nullptr;
    page_free(g_port_dacl);
    g_port_dacl = nullptr;
    ::NtClose(bus.namespace_handle);
    bus.namespace_handle = nullptr;
    ::RtlDeleteBoundaryDescriptor(boundary);
    bus.boundary = nullptr;
    return -1;
  }

  return 0;
}

void fini() {
  AlpcBusState &bus = g_pcb.alpc_bus;

  // Deregister from the reactor first — after unwatch() returns, no
  // callback is running and none will fire, so closing the port below is
  // race-free.
  if (g_reactor_token.valid()) {
    reactor::unwatch(g_reactor_token);
    g_reactor_token = reactor::INVALID_TOKEN;
  }

  if (bus.port) {
    ::NtClose(bus.port);
    bus.port = nullptr;
  }

  if (bus.namespace_handle) {
    ::NtClose(bus.namespace_handle);
    bus.namespace_handle = nullptr;
  }

  if (g_port_dacl) {
    page_free(g_port_dacl);
    g_port_dacl = nullptr;
  }

  if (bus.boundary) {
    ::RtlDeleteBoundaryDescriptor(
        static_cast<POBJECT_BOUNDARY_DESCRIPTOR>(bus.boundary));
    bus.boundary = nullptr;
  }
}

void fork_reinit() {
  AlpcBusState &bus = g_pcb.alpc_bus;

  // The parent's port handle was created with named_internal_oa()
  // (OBJ_KERNEL_HANDLE, non-inheritable), so it never made it into the
  // child's handle table. Null the stale pointer and build fresh state.
  bus.port = nullptr;
  g_reactor_token = reactor::INVALID_TOKEN;

  // The private namespace handle is still valid — same user, same
  // boundary descriptor — so we reuse it. Re-query create_time because
  // the child has a different one (different process identity).
  bus.create_time = query_create_time(NtCurrentProcess());

  if (!bus.namespace_handle)
    return; // bus was never up in the parent; stay down.

  bus.port = create_bus_port(bus.namespace_handle, &g_port_sd, &g_port_attrs,
                             static_cast<pid_t>(NtCurrentProcessId()),
                             bus.create_time);
  if (bus.port) {
    g_reactor_token =
        reactor::watch_alpc(bus.port, reactor_callback, nullptr);
  }
}

void pre_exec_cleanup() {
  AlpcBusState &bus = g_pcb.alpc_bus;

  if (g_reactor_token.valid()) {
    reactor::unwatch(g_reactor_token);
    g_reactor_token = reactor::INVALID_TOKEN;
  }

  if (bus.port) {
    ::NtClose(bus.port);
    bus.port = nullptr;
  }
}

uint64_t self_create_time() { return g_pcb.alpc_bus.create_time; }

bool is_up() { return g_pcb.alpc_bus.port != nullptr; }

// ---------------------------------------------------------------------------
// Peer privilege helper exposed to consumers that still need the tri-state
// send-side check POSIX defines for signals. This is intentionally the only
// function that reaches outside the generic request/reply pattern — other
// subsystems either do their own auth or don't need one.
// ---------------------------------------------------------------------------

namespace peer {

// Returns 0 if the current process may send to `target_pid` under the
// classic POSIX kill() permission model (same real/effective UID, or
// CAP_KILL equivalent). Returns -ESRCH if target doesn't exist, -EPERM
// otherwise. Negative errno values so callers can propagate directly.
int check_signal_send_permission(pid_t target_pid) {
  windows::ScopedNtHandle target_proc;
  NTSTATUS st = ::NtOpenProcessById(target_proc.put(),
                                    PROCESS_QUERY_LIMITED_INFORMATION,
                                    static_cast<DWORD>(target_pid));
  if (!NT_SUCCESS(st))
    return -3; // -ESRCH — avoid pulling in errno.h for a tiny enum.

  windows::ScopedNtHandle target_token;
  st = ::NtOpenProcessTokenEx(target_proc.get(), TOKEN_QUERY, 0,
                              target_token.put());
  if (!NT_SUCCESS(st))
    return -1; // -EPERM

  alignas(8) UCHAR target_buf[256];
  SID *target_sid = query_token_user_sid(target_token.get(), target_buf,
                                         sizeof(target_buf));
  if (!target_sid)
    return -1;

  alignas(8) UCHAR our_buf[256];
  SID *our_sid =
      query_token_user_sid(NtCurrentProcessToken(), our_buf, sizeof(our_buf));
  if (!our_sid)
    return -1;

  if (::RtlEqualSid(our_sid, target_sid))
    return 0;

  windows::ScopedNtHandle thread_token;
  st = ::NtOpenThreadTokenEx(NtCurrentThread(), TOKEN_QUERY, TRUE, 0,
                             thread_token.put());
  if (NT_SUCCESS(st)) {
    alignas(8) UCHAR eff_buf[256];
    SID *eff_sid = query_token_user_sid(thread_token.get(), eff_buf,
                                        sizeof(eff_buf));
    if (eff_sid && ::RtlEqualSid(eff_sid, target_sid))
      return 0;
  }

  if (token_has_debug_privilege(NtCurrentProcessToken()))
    return 0;

  return -1;
}

} // namespace peer

} // namespace alpc_bus
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
