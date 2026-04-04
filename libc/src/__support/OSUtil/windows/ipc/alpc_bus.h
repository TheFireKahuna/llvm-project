//===-- Generic ALPC bus — multiplexed cross-process IPC ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Process-wide ALPC service that multiplexes opcode-tagged request/reply
// traffic between NT-POSIX libc processes. Each consumer (signal delivery,
// fcntl lock ownership attestation, future subsystems) registers a handler
// for one opcode at subsystem init, and sends by calling `request()`.
//
// The bus itself owns:
//   - the private namespace (per-user SID + medium integrity boundary)
//   - the per-process ALPC port and its DACL
//   - reactor registration / drain callback
//   - opcode → handler dispatch table
//
// Security properties (inherited from the original signal transport):
//   1. Unforgeable sender identity — kernel-filled PORT_MESSAGE.ClientId
//   2. DACL-enforced port access — kernel evaluates at connect time
//   3. Private namespace prevents port enumeration
//   4. PID-reuse hardening — sender create_time revalidated on receive
//   5. Per-consumer defense in depth — handlers can do additional checks
//
// Wire format (carried as the ALPC connection-request payload):
//   PORT_MESSAGE header   (kernel-filled)
//   BusHeader             (magic, version, opcode, payload_len, flags,
//                          sender_create_time)
//   payload[payload_len]  (opcode-specific, up to MAX_PAYLOAD bytes)
//
// Reply format is identical: same BusHeader shape, opcode echoed back, with
// the handler's NTSTATUS in `flags_or_status`.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_ALPC_BUS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_ALPC_BUS_H

#include "hdr/stdint_proxy.h"
#include "hdr/types/pid_t.h"
#include "hdr/types/uid_t.h"
#include "src/__support/OSUtil/windows/nt/nt_ipc_types.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace alpc_bus {

// ---------------------------------------------------------------------------
// Wire format
// ---------------------------------------------------------------------------

inline constexpr uint32_t BUS_MAGIC = 0x5355424E;   // 'NBUS' in LE
inline constexpr uint16_t BUS_VERSION = 1;

// Largest opcode payload carried on the bus. Tuned to keep the full frame
// (PORT_MESSAGE + BusHeader + payload) within ALPC's small-message threshold
// so no data view / section lifetime management is ever required.
inline constexpr uint32_t BUS_MAX_PAYLOAD = 192;

// Opcode 0 is reserved (invalid). Consumers pick from the registry below
// so there is one authoritative list of which opcode belongs to whom.
enum OpCode : uint16_t {
  OP_INVALID = 0,
  OP_SIGNAL_DELIVER = 1,    // signal_state::alpc_transport
  OP_LOCK_QUERY = 2,        // internal::fcntl_lock_table
  // Future consumers add opcodes here. Do not reuse retired opcodes.
};

struct BusHeader {
  uint32_t magic;              // BUS_MAGIC
  uint16_t version;            // BUS_VERSION
  uint16_t opcode;             // OpCode
  uint32_t payload_len;        // bytes of payload following this header
  int32_t  flags_or_status;    // request: flags (0); reply: NTSTATUS
  uint64_t sender_create_time; // revalidated by receiver — rejects PID reuse
};

static_assert(sizeof(BusHeader) == 24,
              "BusHeader layout is part of the wire format");

// Full on-wire frame. Fixed size so one buffer services any opcode.
struct BusFrame {
  PORT_MESSAGE port_header;
  BusHeader    bus_header;
  uint8_t      payload[BUS_MAX_PAYLOAD];
};

// ---------------------------------------------------------------------------
// Handler API
// ---------------------------------------------------------------------------

// Identity of the peer that originated a request. Filled by the bus
// dispatcher before the handler runs:
//   - `pid` and `create_time` are kernel-attested (not trusted peer input).
//   - `uid` is resolved from the sender token (or -1 if resolution failed).
struct SenderInfo {
  pid_t    pid;
  uid_t    uid;
  uint64_t create_time;
};

// View of the request payload passed to a handler. `data` points into the
// bus's receive buffer and is only valid for the duration of the handler
// call — copy anything that needs to outlive it.
struct RequestView {
  const void *data;
  uint32_t    size;
};

// Mutable view of the reply buffer a handler fills in. The handler writes
// up to `capacity` bytes into `data` and stores the reply length in
// `*size_out`. On return the dispatcher copies `*size_out` bytes into the
// outgoing reply frame.
struct ReplyBuffer {
  void     *data;
  uint32_t  capacity;
  uint32_t *size_out;
};

// Handler signature. Returns an NTSTATUS-style code which is echoed back
// to the sender in `BusHeader.flags_or_status`. STATUS_SUCCESS → request
// succeeded and `*size_out` bytes of reply are valid; any other value
// → the reply body is ignored by the sender.
using Handler = int32_t (*)(const RequestView &request,
                            const SenderInfo &sender,
                            const ReplyBuffer &reply);

// Register a handler for `op`. Must be called exactly once per opcode
// across the lifetime of the process (the dispatcher rejects double
// registrations). Safe to call before or after `init()` — handlers are
// stored in a static table and consulted on every dispatch.
//
// Returns 0 on success, -1 if the opcode is out of range or already
// registered.
int register_handler(uint16_t op, Handler handler);

// ---------------------------------------------------------------------------
// Request / reply (client side)
// ---------------------------------------------------------------------------

// Send `payload_len` bytes tagged with `opcode` to the libc process
// identified by `target_pid` and synchronously wait for a reply.
//
// The sender's identity is kernel-attested by ALPC. PID-reuse is
// prevented by embedding the caller's create_time in every message and
// revalidating on the receiver.
//
// If `reply_buf != nullptr`, up to `reply_cap` bytes of reply payload are
// copied into it and the actual length is written to `*reply_len`.
//
// `timeout_100ns` follows the NT convention: negative = relative timeout
// in 100ns units (e.g. -5000000 = 500ms). 0 means no wait. Positive
// values are absolute kernel times — almost always wrong; stick to
// negative values.
//
// Returns the handler's NTSTATUS on success, or a negative STATUS_* value
// if the connect/send itself failed (target not found, port closed,
// timeout, ACL rejection, bad reply frame, etc.).
int32_t request(pid_t target_pid, uint16_t opcode,
                const void *payload, uint32_t payload_len,
                void *reply_buf, uint32_t reply_cap, uint32_t *reply_len,
                int64_t timeout_100ns);

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Bring up the bus: create the boundary descriptor, open/create the
// private namespace, build the DACL, create the ALPC port, register with
// the process-wide reactor. Returns 0 on success, -1 on any failure
// (cross-process IPC is then disabled but in-process operations still
// work — every consumer's public API must tolerate a missing bus).
int init();

// Tear down the bus. Called from the subsystem fini phase.
void fini();

// Rebuild the bus in the fork child. The parent's port handle is not
// inherited (OBJ_KERNEL_HANDLE), so this creates a fresh port named after
// the child's PID + create_time and re-registers it with the reactor
// (which was already rebuilt earlier in the fork reinit chain).
void fork_reinit();

// Close the port before exec so the replacement image's CRT init can
// re-create it cleanly. Safe to call even if the bus was never brought
// up (no-ops in that case).
void pre_exec_cleanup();

// ---------------------------------------------------------------------------
// Misc helpers exposed for consumers that need them
// ---------------------------------------------------------------------------

// Returns the calling process's create_time as recorded when the bus was
// initialized (or 0 if the bus is not up). Consumers that build payloads
// referencing their own identity (e.g. signal value routing) read this
// instead of querying the kernel every time.
uint64_t self_create_time();

// True once `init()` or `fork_reinit()` has produced a live port.
bool is_up();

// ---------------------------------------------------------------------------
// Peer authorization helpers
// ---------------------------------------------------------------------------
//
// The original signal transport carried a POSIX kill() permission model
// (same real/effective UID or CAP_KILL equivalent). Now that the port
// lifecycle is shared, the permission helper moves here so every consumer
// that wants the same policy can use it directly. Subsystems with their
// own authorization rules just skip this namespace.

namespace peer {

// Returns 0 if the caller may signal `target_pid` under POSIX rules,
// -3 if the target does not exist (POSIX ESRCH), -1 otherwise (POSIX
// EPERM). Negative values are returned as small integers so this header
// does not depend on errno macros — the one caller that cares maps them
// back to errno codes at its boundary.
int check_signal_send_permission(pid_t target_pid);

} // namespace peer

} // namespace alpc_bus
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_ALPC_BUS_H
