//===-- Internal identity operations for Windows -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Engine functions for setuid, seteuid, setreuid, setgid, setegid, setregid.
// Each returns 0 on success, -errno on failure.
//
//===----------------------------------------------------------------------===//

#include "identity_ops.h"

#include "hdr/errno_macros.h"
#include "hdr/types/gid_t.h"
#include "hdr/types/size_t.h"
#include "hdr/types/uid_t.h"
#include "src/__support/OSUtil/windows/alloc/page_alloc.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/OSUtil/windows/nt/nt_string_types.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process/process_identity.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/process/sid_utils.h"
#include "src/__support/OSUtil/windows/signal/signal_internal.h"
#include "src/__support/threads/windows/thread_registry.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

namespace {

/// Suspend all registered threads except `self_tid`.
void suspend_all_threads(DWORD self_tid) {
  registry_for_each([](ThreadLifecycle *node) -> bool {
    registry_suspend(node);
    return false;
  }, self_tid);
}

/// Apply an impersonation token to all threads except `self_tid`, then resume.
/// If `token` is null, clears impersonation instead.
void apply_token_and_resume_all(DWORD self_tid, HANDLE token) {
  registry_for_each([&](ThreadLifecycle *node) -> bool {
    HANDLE h = registry_borrow_handle(node);
    if (h) {
      if (token)
        windows_identity::apply_impersonation(h, token);
      else
        windows_identity::clear_impersonation(h);
    }
    // Always resume — suspend_all_threads suspended this thread regardless
    // of handle availability. Skipping resume would leave it stuck.
    registry_resume(node);
    return false;
  }, self_tid);
}

} // namespace

// ---- getlogin engine -------------------------------------------------------
//
// Retrieves the login name (username) for the current effective user.
// Uses the effective UID → SID → RtlConvertSidToUnicodeString path from
// sid_utils.h (which strips the domain prefix), then converts UTF-16 → UTF-8
// via RtlUnicodeToUTF8N.

intptr_t getlogin(char *buf, size_t bufsize) {
  uid_t euid = g_pcb.identity.eff_uid.load(cpp::MemoryOrder::RELAXED);

  // Get the wide username (domain-stripped) from the SID.
  WCHAR wname[256];
  size_t wlen = uid_to_username(euid, wname, 256);
  if (wlen == 0)
    return -ENOENT;

  // Convert UTF-16 → UTF-8.
  int utf8_len = windows::wide_to_utf8_n(wname, wlen * sizeof(WCHAR), buf,
                                         static_cast<int>(bufsize));
  if (utf8_len < 0)
    return -ERANGE;

  // Ensure NUL termination.
  if (static_cast<size_t>(utf8_len) >= bufsize)
    return -ERANGE;
  buf[utf8_len] = '\0';

  return static_cast<intptr_t>(utf8_len);
}

// ---- seteuid engine --------------------------------------------------------

intptr_t seteuid(uid_t euid) {
  uid_t cur_euid = g_pcb.identity.eff_uid.load(cpp::MemoryOrder::RELAXED);
  if (euid == cur_euid)
    return 0; // No-op.

  uid_t real = g_pcb.identity.real_uid.load(cpp::MemoryOrder::RELAXED);
  uid_t saved = g_pcb.identity.saved_uid.load(cpp::MemoryOrder::RELAXED);
  uint8_t priv =
      g_pcb.identity.privilege_level.load(cpp::MemoryOrder::RELAXED);

  // Unprivileged: only allow real_uid or saved_uid.
  if (priv < windows_identity::PRIV_TCB && euid != real && euid != saved)
    return -EPERM;

  HANDLE new_token = nullptr;

  if (euid == real) {
    // Reverting to real identity — clear impersonation.
    new_token = nullptr;
  } else {
    // Acquire a token for the target uid via S4U logon.
    NTSTATUS status =
        windows_identity::acquire_token_for_uid(euid, &new_token);
    if (!NT_SUCCESS(status)) {
      if (status == STATUS_PRIVILEGE_NOT_HELD)
        return -EPERM;
      else if (status == STATUS_NO_SUCH_USER)
        return -EINVAL;
      else
        return -EPERM;
    }
  }

  // Swap the impersonation token.
  HANDLE old_token =
      g_pcb.identity.impersonation_token.exchange(new_token,
                                                  cpp::MemoryOrder::ACQ_REL);

  // Update effective uid.
  g_pcb.identity.eff_uid.store(euid, cpp::MemoryOrder::RELEASE);

  // Suspend-all-then-apply: freeze every other thread, swap tokens, resume.
  // A thread that registers between pass 1 and pass 2 would briefly run
  // with the old effective UID. Re-check the registry count to detect this
  // and suspend any newcomers before applying tokens.
  DWORD self = NtCurrentThreadId();

  // Pass 1: suspend all other threads.
  uint32_t count_before = registry_live_count();
  suspend_all_threads(self);

  // Check for threads that registered between pass 1 start and end.
  // If new threads appeared, suspend them too before applying tokens.
  uint32_t count_after = registry_live_count();
  if (count_after != count_before)
    suspend_all_threads(self);

  // Pass 2: set token and resume.
  apply_token_and_resume_all(self, new_token);

  // Apply to the calling thread.
  if (new_token)
    windows_identity::apply_impersonation(NtCurrentThread(), new_token);
  else
    windows_identity::clear_impersonation(NtCurrentThread());

  // Close the old token after all threads have been updated.
  if (old_token)
    ::NtClose(old_token);

  return 0;
}

// ---- setuid engine ---------------------------------------------------------

intptr_t setuid(uid_t uid) {
  uint8_t priv =
      g_pcb.identity.privilege_level.load(cpp::MemoryOrder::RELAXED);

  if (priv < windows_identity::PRIV_TCB) {
    // Unprivileged: setuid(uid) is equivalent to seteuid(uid).
    return internal::seteuid(uid);
  }

  // Privileged path: set all three IDs permanently.
  uid_t cur_euid = g_pcb.identity.eff_uid.load(cpp::MemoryOrder::RELAXED);

  if (uid == cur_euid &&
      uid == g_pcb.identity.real_uid.load(cpp::MemoryOrder::RELAXED) &&
      uid == g_pcb.identity.saved_uid.load(cpp::MemoryOrder::RELAXED)) {
    return 0; // Already this identity, no-op.
  }

  // Acquire token for the new uid.
  HANDLE new_token = nullptr;
  uid_t real = g_pcb.identity.real_uid.load(cpp::MemoryOrder::RELAXED);

  if (uid != real) {
    NTSTATUS status =
        windows_identity::acquire_token_for_uid(uid, &new_token);
    if (!NT_SUCCESS(status)) {
      if (status == STATUS_PRIVILEGE_NOT_HELD)
        return -EPERM;
      else if (status == STATUS_NO_SUCH_USER)
        return -EINVAL;
      else
        return -EPERM;
    }
  }

  // Swap impersonation token.
  HANDLE old_token =
      g_pcb.identity.impersonation_token.exchange(new_token,
                                                  cpp::MemoryOrder::ACQ_REL);

  // Set all three IDs — this makes the change permanent.
  g_pcb.identity.real_uid.store(uid, cpp::MemoryOrder::RELAXED);
  g_pcb.identity.eff_uid.store(uid, cpp::MemoryOrder::RELEASE);
  g_pcb.identity.saved_uid.store(uid, cpp::MemoryOrder::RELAXED);

  // Suspend-all-then-apply for atomic identity change.
  DWORD self = NtCurrentThreadId();

  // Pass 1: suspend all other threads.
  uint32_t count_before = registry_live_count();
  suspend_all_threads(self);

  // Re-check for threads that registered during pass 1.
  uint32_t count_after = registry_live_count();
  if (count_after != count_before)
    suspend_all_threads(self);

  // Pass 2: set token and resume.
  apply_token_and_resume_all(self, new_token);

  if (new_token)
    windows_identity::apply_impersonation(NtCurrentThread(), new_token);
  else
    windows_identity::clear_impersonation(NtCurrentThread());

  // Drop privilege level AFTER all threads have the new token.
  // Storing before the suspend-apply would create a TOCTOU window where
  // a thread waking between the store and token swap sees PRIV_NONE
  // with the old token.
  if (uid != real)
    g_pcb.identity.privilege_level.store(windows_identity::PRIV_NONE,
                                         cpp::MemoryOrder::RELEASE);

  if (old_token)
    ::NtClose(old_token);

  return 0;
}

// ---- setreuid engine -------------------------------------------------------
//
// POSIX setreuid(ruid, euid) semantics:
//   - If ruid is not (uid_t)-1, set real UID to ruid.
//   - If euid is not (uid_t)-1, set effective UID to euid.
//   - If the real UID is set (or the effective UID is set to a value
//     different from the previous real UID), the saved-set-UID is set
//     to the new effective UID.
//   - Unprivileged: ruid must be current real or effective; euid must be
//     current real, effective, or saved.
//
// We perform the token acquisition before mutating any state, so failure
// is clean (no partial update). The suspend-all-then-apply pattern from
// seteuid is used to push the new token to all threads atomically.

intptr_t setreuid(uid_t ruid, uid_t euid) {
  uid_t cur_real = g_pcb.identity.real_uid.load(cpp::MemoryOrder::RELAXED);
  uid_t cur_euid = g_pcb.identity.eff_uid.load(cpp::MemoryOrder::RELAXED);
  uid_t cur_saved = g_pcb.identity.saved_uid.load(cpp::MemoryOrder::RELAXED);
  uint8_t priv =
      g_pcb.identity.privilege_level.load(cpp::MemoryOrder::RELAXED);

  uid_t new_real = (ruid == static_cast<uid_t>(-1)) ? cur_real : ruid;
  uid_t new_euid = (euid == static_cast<uid_t>(-1)) ? cur_euid : euid;

  // No-op fast path.
  if (new_real == cur_real && new_euid == cur_euid)
    return 0;

  // Permission checks for unprivileged processes.
  if (priv < windows_identity::PRIV_TCB) {
    // ruid: must be current real or effective.
    if (ruid != static_cast<uid_t>(-1) && ruid != cur_real && ruid != cur_euid)
      return -EPERM;
    // euid: must be current real, effective, or saved.
    if (euid != static_cast<uid_t>(-1) && euid != cur_real &&
        euid != cur_euid && euid != cur_saved)
      return -EPERM;
  }

  // Acquire token for the new effective UID before mutating state.
  HANDLE new_token = nullptr;
  bool need_token_change = (new_euid != cur_euid);
  bool clear_token = (new_euid == new_real);

  if (need_token_change && !clear_token) {
    NTSTATUS status =
        windows_identity::acquire_token_for_uid(new_euid, &new_token);
    if (!NT_SUCCESS(status)) {
      if (status == STATUS_NO_SUCH_USER)
        return -EINVAL;
      return -EPERM;
    }
  }

  // POSIX: if the real UID was set, or the effective UID was set to a
  // value not equal to the previous real UID, the saved-set-UID is set
  // to the new effective UID.
  bool update_saved = (ruid != static_cast<uid_t>(-1)) ||
                      (euid != static_cast<uid_t>(-1) && new_euid != cur_real);
  uid_t new_saved = update_saved ? new_euid : cur_saved;

  // If the effective UID changed, swap the impersonation token and push
  // to all threads via suspend-all-then-apply.
  if (need_token_change) {
    HANDLE set_token = clear_token ? nullptr : new_token;
    HANDLE old_token =
        g_pcb.identity.impersonation_token.exchange(set_token,
                                                    cpp::MemoryOrder::ACQ_REL);

    // Update IDs before thread walk — readers see consistent state after
    // the release store on effective_uid.
    g_pcb.identity.real_uid.store(new_real, cpp::MemoryOrder::RELAXED);
    g_pcb.identity.saved_uid.store(new_saved, cpp::MemoryOrder::RELAXED);
    g_pcb.identity.eff_uid.store(new_euid, cpp::MemoryOrder::RELEASE);

    // Suspend-all-then-apply.
    DWORD self = NtCurrentThreadId();
    suspend_all_threads(self);
    apply_token_and_resume_all(self, set_token);

    if (set_token)
      windows_identity::apply_impersonation(NtCurrentThread(), set_token);
    else
      windows_identity::clear_impersonation(NtCurrentThread());

    if (old_token)
      ::NtClose(old_token);
  } else {
    // Only the real and/or saved UIDs changed — no token work needed.
    g_pcb.identity.real_uid.store(new_real, cpp::MemoryOrder::RELAXED);
    g_pcb.identity.saved_uid.store(new_saved, cpp::MemoryOrder::RELEASE);
  }

  return 0;
}

// ---- getgroups engine ------------------------------------------------------
//
// POSIX getgroups(size, list):
//   - If size == 0, return the number of supplementary group IDs.
//   - If size > 0 and >= ngroups, fill list[] and return ngroups.
//   - If size > 0 and < ngroups, return -EINVAL.
//
// We query the active token's group list via NtQueryInformationToken(TokenGroups)
// and filter to groups that are SE_GROUP_ENABLED (active for access checks),
// excluding the primary group (already reported by getgid/getegid), logon SIDs,
// integrity SIDs, and deny-only SIDs per POSIX convention.

intptr_t getgroups(int size, gid_t list[]) {
  // Use the active token (impersonation if set, otherwise process).
  HANDLE token =
      g_pcb.identity.impersonation_token.load(cpp::MemoryOrder::ACQUIRE);
  HANDLE target = token ? token : NtCurrentProcessToken();

  // First query to get required buffer size.
  ULONG needed = 0;
  NTSTATUS status = ::NtQueryInformationToken(
      target, TokenGroups, nullptr, 0, &needed);
  if (status != STATUS_BUFFER_TOO_SMALL)
    return -EINVAL;

  // Scratch buffer for typical group counts (< ~30 groups). Fall back to
  // page_alloc for tokens with unusually many groups.
  auto tg_s = internal::byte_scratch(1024);
  void *heap_buf = nullptr;
  void *buf;
  if (!tg_s)
    return -ENOMEM;
  if (needed <= tg_s.size()) {
    buf = tg_s.data();
  } else {
    heap_buf = internal::page_alloc(static_cast<size_t>(needed));
    if (!heap_buf)
      return -ENOMEM;
    buf = heap_buf;
  }

  status = ::NtQueryInformationToken(target, TokenGroups, buf, needed, &needed);
  if (!NT_SUCCESS(status)) {
    if (heap_buf)
      internal::page_free(heap_buf);
    return -EINVAL;
  }

  auto *groups = reinterpret_cast<TOKEN_GROUPS *>(buf);

  // Get the primary group GID so we can exclude it from supplementary list.
  gid_t primary_gid = g_pcb.identity.eff_gid.load(cpp::MemoryOrder::RELAXED);

  // Count qualifying groups and optionally fill the list.
  int count = 0;
  for (ULONG i = 0; i < groups->GroupCount; ++i) {
    ULONG attrs = groups->Groups[i].Attributes;

    // Skip groups that are not enabled for access checks.
    if (!(attrs & SE_GROUP_ENABLED))
      continue;

    // Skip logon session SIDs, integrity SIDs, and deny-only SIDs —
    // these are Windows-specific and not meaningful as POSIX supplementary groups.
    if (attrs & (SE_GROUP_LOGON_ID | SE_GROUP_INTEGRITY |
                 SE_GROUP_USE_FOR_DENY_ONLY))
      continue;

    gid_t gid = static_cast<gid_t>(
        internal::sid_to_uid(groups->Groups[i].Sid));

    // Skip the primary group — POSIX supplementary list doesn't include it.
    if (gid == primary_gid)
      continue;

    if (size > 0) {
      if (count >= size) {
        if (heap_buf)
          internal::page_free(heap_buf);
        return -EINVAL;
      }
      list[count] = gid;
    }
    ++count;
  }

  if (heap_buf)
    internal::page_free(heap_buf);
  return static_cast<intptr_t>(count);
}

// ---- setegid engine --------------------------------------------------------

intptr_t setegid(gid_t egid) {
  gid_t cur_egid = g_pcb.identity.eff_gid.load(cpp::MemoryOrder::RELAXED);
  if (egid == cur_egid)
    return 0;

  gid_t real = g_pcb.identity.real_gid.load(cpp::MemoryOrder::RELAXED);
  gid_t saved = g_pcb.identity.saved_gid.load(cpp::MemoryOrder::RELAXED);
  uint8_t priv =
      g_pcb.identity.privilege_level.load(cpp::MemoryOrder::RELAXED);

  if (priv < windows_identity::PRIV_TCB && egid != real && egid != saved)
    return -EPERM;

  // Build SID for the new group.
  alignas(8) UCHAR sid_buf[internal::MAX_SID_SIZE];
  auto *gid_sid = reinterpret_cast<SID *>(sid_buf);
  if (!internal::gid_to_sid(egid, gid_sid))
    return -EINVAL;

  TOKEN_PRIMARY_GROUP tpg;
  tpg.PrimaryGroup = gid_sid;

  // Modify the active token's primary group.
  HANDLE token =
      g_pcb.identity.impersonation_token.load(cpp::MemoryOrder::ACQUIRE);
  HANDLE target = token ? token : NtCurrentProcessToken();
  NTSTATUS status = ::NtSetInformationToken(
      target, TokenPrimaryGroup, &tpg, sizeof(tpg));
  if (!NT_SUCCESS(status))
    return -EPERM;

  g_pcb.identity.eff_gid.store(egid, cpp::MemoryOrder::RELEASE);
  return 0;
}

// ---- setgid engine ---------------------------------------------------------

intptr_t setgid(gid_t gid) {
  uint8_t priv =
      g_pcb.identity.privilege_level.load(cpp::MemoryOrder::RELAXED);

  if (priv < windows_identity::PRIV_TCB)
    return internal::setegid(gid);

  // Privileged path: call setegid for the token change, then set all three.
  intptr_t ret = internal::setegid(gid);
  if (ret != 0)
    return ret;

  g_pcb.identity.real_gid.store(gid, cpp::MemoryOrder::RELAXED);
  g_pcb.identity.saved_gid.store(gid, cpp::MemoryOrder::RELAXED);
  // effective_gid already set by setegid.

  return 0;
}

// ---- setregid engine -------------------------------------------------------
//
// POSIX setregid(rgid, egid) semantics:
//   - If rgid is not (gid_t)-1, set real GID to rgid.
//   - If egid is not (gid_t)-1, set effective GID to egid.
//   - If the real GID was set, or the effective GID was set to a value
//     not equal to the previous real GID, the saved-set-GID is set to
//     the new effective GID.
//   - Unprivileged: rgid must be current real or effective; egid must be
//     current real, effective, or saved.
//
// GID changes don't require the suspend-all-then-apply pattern because
// the primary group is a token attribute, not a per-thread impersonation
// state. NtSetInformationToken(TokenPrimaryGroup) is process-wide.

intptr_t setregid(gid_t rgid, gid_t egid) {
  gid_t cur_real = g_pcb.identity.real_gid.load(cpp::MemoryOrder::RELAXED);
  gid_t cur_egid = g_pcb.identity.eff_gid.load(cpp::MemoryOrder::RELAXED);
  gid_t cur_saved = g_pcb.identity.saved_gid.load(cpp::MemoryOrder::RELAXED);
  uint8_t priv =
      g_pcb.identity.privilege_level.load(cpp::MemoryOrder::RELAXED);

  gid_t new_real = (rgid == static_cast<gid_t>(-1)) ? cur_real : rgid;
  gid_t new_egid = (egid == static_cast<gid_t>(-1)) ? cur_egid : egid;

  // No-op fast path.
  if (new_real == cur_real && new_egid == cur_egid)
    return 0;

  // Permission checks for unprivileged processes.
  if (priv < windows_identity::PRIV_TCB) {
    if (rgid != static_cast<gid_t>(-1) && rgid != cur_real && rgid != cur_egid)
      return -EPERM;
    if (egid != static_cast<gid_t>(-1) && egid != cur_real &&
        egid != cur_egid && egid != cur_saved)
      return -EPERM;
  }

  // POSIX saved-set-GID rule (same logic as setreuid).
  bool update_saved = (rgid != static_cast<gid_t>(-1)) ||
                      (egid != static_cast<gid_t>(-1) && new_egid != cur_real);
  gid_t new_saved = update_saved ? new_egid : cur_saved;

  // Apply the effective GID change via the token's primary group.
  if (new_egid != cur_egid) {
    alignas(8) UCHAR sid_buf[internal::MAX_SID_SIZE];
    auto *gid_sid = reinterpret_cast<SID *>(sid_buf);
    if (!internal::gid_to_sid(new_egid, gid_sid))
      return -EINVAL;

    TOKEN_PRIMARY_GROUP tpg;
    tpg.PrimaryGroup = gid_sid;

    HANDLE token =
        g_pcb.identity.impersonation_token.load(cpp::MemoryOrder::ACQUIRE);
    HANDLE target = token ? token : NtCurrentProcessToken();
    NTSTATUS status = ::NtSetInformationToken(
        target, TokenPrimaryGroup, &tpg, sizeof(tpg));
    if (!NT_SUCCESS(status))
      return -EPERM;
  }

  // Commit all three GIDs. Order: real and saved before the release store
  // on effective_gid, so concurrent readers via getegid see a consistent
  // snapshot after observing the new effective_gid.
  g_pcb.identity.real_gid.store(new_real, cpp::MemoryOrder::RELAXED);
  g_pcb.identity.saved_gid.store(new_saved, cpp::MemoryOrder::RELAXED);
  g_pcb.identity.eff_gid.store(new_egid, cpp::MemoryOrder::RELEASE);

  return 0;
}

// ---- getresuid engine ------------------------------------------------------

intptr_t getresuid(uid_t *ruid, uid_t *euid, uid_t *suid) {
  if (ruid)
    *ruid = g_pcb.identity.real_uid.load(cpp::MemoryOrder::RELAXED);
  if (euid)
    *euid = g_pcb.identity.eff_uid.load(cpp::MemoryOrder::RELAXED);
  if (suid)
    *suid = g_pcb.identity.saved_uid.load(cpp::MemoryOrder::RELAXED);
  return 0;
}

// ---- getresgid engine ------------------------------------------------------

intptr_t getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid) {
  if (rgid)
    *rgid = g_pcb.identity.real_gid.load(cpp::MemoryOrder::RELAXED);
  if (egid)
    *egid = g_pcb.identity.eff_gid.load(cpp::MemoryOrder::RELAXED);
  if (sgid)
    *sgid = g_pcb.identity.saved_gid.load(cpp::MemoryOrder::RELAXED);
  return 0;
}

// ---- setresuid engine ------------------------------------------------------
//
// Linux setresuid(ruid, euid, suid) semantics:
//   - Any of the three may be (uid_t)-1 to leave that ID unchanged.
//   - Unprivileged: each specified ID must equal one of the current real,
//     effective, or saved UID.
//   - All changes are atomic — if one fails, none take effect.
//
// setresuid subsumes setreuid and setuid: setreuid(r,e) == setresuid(r,e,-1),
// and setuid(u) for privileged == setresuid(u,u,u).
//
// The key difference from setreuid is explicit control over the saved-set-UID
// (setreuid computes it implicitly from the POSIX saved-set-UID rule).

intptr_t setresuid(uid_t ruid, uid_t euid, uid_t suid) {
  uid_t cur_real = g_pcb.identity.real_uid.load(cpp::MemoryOrder::RELAXED);
  uid_t cur_euid = g_pcb.identity.eff_uid.load(cpp::MemoryOrder::RELAXED);
  uid_t cur_saved = g_pcb.identity.saved_uid.load(cpp::MemoryOrder::RELAXED);
  uint8_t priv =
      g_pcb.identity.privilege_level.load(cpp::MemoryOrder::RELAXED);

  uid_t new_real = (ruid == static_cast<uid_t>(-1)) ? cur_real : ruid;
  uid_t new_euid = (euid == static_cast<uid_t>(-1)) ? cur_euid : euid;
  uid_t new_saved = (suid == static_cast<uid_t>(-1)) ? cur_saved : suid;

  // No-op fast path.
  if (new_real == cur_real && new_euid == cur_euid && new_saved == cur_saved)
    return 0;

  // Permission checks for unprivileged processes: each specified ID must
  // equal one of the current {real, effective, saved}.
  if (priv < windows_identity::PRIV_TCB) {
    auto allowed = [cur_real, cur_euid, cur_saved](uid_t v) {
      return v == cur_real || v == cur_euid || v == cur_saved;
    };
    if (ruid != static_cast<uid_t>(-1) && !allowed(ruid))
      return -EPERM;
    if (euid != static_cast<uid_t>(-1) && !allowed(euid))
      return -EPERM;
    if (suid != static_cast<uid_t>(-1) && !allowed(suid))
      return -EPERM;
  }

  // Token acquisition before mutation — clean failure semantics.
  HANDLE new_token = nullptr;
  bool need_token_change = (new_euid != cur_euid);
  bool clear_token = (new_euid == new_real);

  if (need_token_change && !clear_token) {
    NTSTATUS status =
        windows_identity::acquire_token_for_uid(new_euid, &new_token);
    if (!NT_SUCCESS(status)) {
      if (status == STATUS_NO_SUCH_USER)
        return -EINVAL;
      return -EPERM;
    }
  }

  if (need_token_change) {
    HANDLE set_token = clear_token ? nullptr : new_token;
    HANDLE old_token =
        g_pcb.identity.impersonation_token.exchange(set_token,
                                                    cpp::MemoryOrder::ACQ_REL);

    // Store all three IDs before the thread walk.
    g_pcb.identity.real_uid.store(new_real, cpp::MemoryOrder::RELAXED);
    g_pcb.identity.saved_uid.store(new_saved, cpp::MemoryOrder::RELAXED);
    g_pcb.identity.eff_uid.store(new_euid, cpp::MemoryOrder::RELEASE);

    // Suspend-all-then-apply.
    DWORD self = NtCurrentThreadId();
    suspend_all_threads(self);
    apply_token_and_resume_all(self, set_token);

    if (set_token)
      windows_identity::apply_impersonation(NtCurrentThread(), set_token);
    else
      windows_identity::clear_impersonation(NtCurrentThread());

    if (old_token)
      ::NtClose(old_token);
  } else {
    // Only real and/or saved changed — no token work.
    g_pcb.identity.real_uid.store(new_real, cpp::MemoryOrder::RELAXED);
    g_pcb.identity.saved_uid.store(new_saved, cpp::MemoryOrder::RELEASE);
  }

  return 0;
}

// ---- setresgid engine ------------------------------------------------------
//
// Linux setresgid(rgid, egid, sgid) semantics — same pattern as setresuid
// but for GIDs. GID changes are process-wide (TokenPrimaryGroup), so no
// suspend-all-then-apply is needed.

intptr_t setresgid(gid_t rgid, gid_t egid, gid_t sgid) {
  gid_t cur_real = g_pcb.identity.real_gid.load(cpp::MemoryOrder::RELAXED);
  gid_t cur_egid = g_pcb.identity.eff_gid.load(cpp::MemoryOrder::RELAXED);
  gid_t cur_saved = g_pcb.identity.saved_gid.load(cpp::MemoryOrder::RELAXED);
  uint8_t priv =
      g_pcb.identity.privilege_level.load(cpp::MemoryOrder::RELAXED);

  gid_t new_real = (rgid == static_cast<gid_t>(-1)) ? cur_real : rgid;
  gid_t new_egid = (egid == static_cast<gid_t>(-1)) ? cur_egid : egid;
  gid_t new_saved = (sgid == static_cast<gid_t>(-1)) ? cur_saved : sgid;

  // No-op fast path.
  if (new_real == cur_real && new_egid == cur_egid && new_saved == cur_saved)
    return 0;

  // Permission checks for unprivileged processes.
  if (priv < windows_identity::PRIV_TCB) {
    auto allowed = [cur_real, cur_egid, cur_saved](gid_t v) {
      return v == cur_real || v == cur_egid || v == cur_saved;
    };
    if (rgid != static_cast<gid_t>(-1) && !allowed(rgid))
      return -EPERM;
    if (egid != static_cast<gid_t>(-1) && !allowed(egid))
      return -EPERM;
    if (sgid != static_cast<gid_t>(-1) && !allowed(sgid))
      return -EPERM;
  }

  // Apply the effective GID change via the token's primary group.
  if (new_egid != cur_egid) {
    alignas(8) UCHAR sid_buf[internal::MAX_SID_SIZE];
    auto *gid_sid = reinterpret_cast<SID *>(sid_buf);
    if (!internal::gid_to_sid(new_egid, gid_sid))
      return -EINVAL;

    TOKEN_PRIMARY_GROUP tpg;
    tpg.PrimaryGroup = gid_sid;

    HANDLE token =
        g_pcb.identity.impersonation_token.load(cpp::MemoryOrder::ACQUIRE);
    HANDLE target = token ? token : NtCurrentProcessToken();
    NTSTATUS status = ::NtSetInformationToken(
        target, TokenPrimaryGroup, &tpg, sizeof(tpg));
    if (!NT_SUCCESS(status))
      return -EPERM;
  }

  // Commit all three GIDs.
  g_pcb.identity.real_gid.store(new_real, cpp::MemoryOrder::RELAXED);
  g_pcb.identity.saved_gid.store(new_saved, cpp::MemoryOrder::RELAXED);
  g_pcb.identity.eff_gid.store(new_egid, cpp::MemoryOrder::RELEASE);

  return 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
