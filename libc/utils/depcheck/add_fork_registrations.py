#!/usr/bin/env python3
"""
One-shot script to add LIBC_REGISTER_FORK_REINIT(...) to each subsystem TU
that defines a *_fork_reinit() function. Used during the .libcfork registry
migration; safe to delete after the migration lands.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[3]

# (relative path, tag, priority constant, qualified function name)
ENTRIES = [
    # veh_core handled manually (already done — has LIBC_REGISTER_FINI nearby).
    # rdebug handled manually.
    ("libc/src/__support/OSUtil/windows/process/process_identity.cpp",
     "identity", "kForkPrioIdentity", "identity_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/memory/sysv_shm_ops.cpp",
     "sysv_shm", "kForkPrioSysvShm", "sysv_shm_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/concurrent/crystalline_domain.cpp",
     "crystalline", "kForkPrioCrystalline", "crystalline_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/alloc/va_substrate.cpp",
     "va_substrate", "kForkPrioVaSubstrate", "va_substrate_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/memory/mapping_table.cpp",
     "mapping_table", "kForkPrioMappingTable", "mapping_table_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/alloc/thread_scratch.cpp",
     "scratch", "kForkPrioScratch", "scratch_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/security/pkey_ops.cpp",
     "pkey", "kForkPrioPkey", "pkey_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/memory/mmap_engine.cpp",
     "mmap_lock", "kForkPrioMmapLock", "mmap_lock_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/memory/region_reconcile.cpp",
     "memory_reconcile", "kForkPrioMemoryReconcile", "memory_reconcile_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/memory/posix/mlockall.cpp",
     "mlock_policy", "kForkPrioMlockPolicy", "mlock_policy_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/libc_subsystem_init.cpp",
     "va_inventory", "kForkPrioVaInventory", "va_inventory_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/io/env_ops.cpp",
     "env", "kForkPrioEnv", "env_fork_reinit"),
    ("libc/src/stdlib/windows/posix_alloc.cpp",
     "alloc", "kForkPrioAlloc", "alloc_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/fd/ofd_pool.cpp",
     "ofd_pool", "kForkPrioOfdPool", "ofd_pool_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/memory/brk_state.cpp",
     "brk", "kForkPrioBrk", "brk_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/fd/file_pool.cpp",
     "file_pool", "kForkPrioFilePool", "file_pool_fork_reinit"),
    ("libc/src/__support/threads/windows/wait_slot.cpp",
     "wait_slot", "kForkPrioWaitSlot", "wait_slot_fork_reinit"),
    ("libc/src/__support/threads/windows/futex_subsystem_register.cpp",
     "futex_addr", "kForkPrioFutexAddr", "futex_addr_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/time/itimer_ops.cpp",
     "setitimer", "kForkPrioSetitimer", "setitimer_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/time/timer_ops.cpp",
     "timer_create", "kForkPrioTimerCreate", "timer_create_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/resource/cpu_limit_timer.cpp",
     "cpu_limit_timer", "kForkPrioCpuLimitTimer", "cpu_limit_timer_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/resource/cpu_limit_timer.cpp",
     "cpu_limit_timer_restore", "kForkPrioCpuLimitTimerRestore", "cpu_limit_timer_fork_restore"),
    ("libc/src/__support/process/windows/child_table.cpp",
     "child_table", "kForkPrioChildTable", "child_table_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/resource/resource_ops.cpp",
     "rlimit", "kForkPrioRlimit", "rlimit_fork_reinit"),
    ("libc/src/__support/threads/windows/thread_lifecycle.cpp",
     "lifecycle", "kForkPrioLifecycle", "lifecycle_fork_reinit"),
    ("libc/startup/windows/libc_init.cpp",
     "thread_self", "kForkPrioThreadSelf", "thread_self_fork_reinit"),
    ("libc/src/__support/threads/windows/mutex.cpp",
     "robust_pool", "kForkPrioRobustPool", "robust_pool_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/io/thread_ring.cpp",
     "thread_ring", "kForkPrioThreadRing", "thread_ring_fork_reinit"),
    ("libc/src/__support/threads/windows/thread.cpp",
     "thread_storage", "kForkPrioThreadStorage", "thread_storage_fork_reinit"),
    ("libc/src/__support/threads/windows/named_semaphore.cpp",
     "named_semaphore", "kForkPrioNamedSemaphore", "named_semaphore_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/fd/fd_table.cpp",
     "fd_table", "kForkPrioFdTable", "fd_table_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/fcntl_lock_table.cpp",
     "lock_table", "kForkPrioLockTable", "lock_table_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/process/console_tty_session.cpp",
     "console_tty", "kForkPrioConsoleTty", "console_tty_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/reactor/reactor.cpp",
     "reactor", "kForkPrioReactor", "reactor_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/ipc/alpc_bus_subsystem.cpp",
     "alpc_bus", "kForkPrioAlpcBus", "alpc_bus_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/signal/signal_state.cpp",
     "signal", "kForkPrioSignal", "signal_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/ipc/epoll_ops.cpp",
     "epoll", "kForkPrioEpoll", "epoll_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/ipc/inotify_ops.cpp",
     "inotify", "kForkPrioInotify", "inotify_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/dlfcn/dlfcn_ops.cpp",
     "dlfcn", "kForkPrioDlfcn", "dlfcn_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/process/pty_tree.cpp",
     "pty_tree", "kForkPrioPtyTree", "pty_tree_fork_reinit"),
    ("libc/src/__support/OSUtil/windows/process/vt_pty.cpp",
     "vt_pty", "kForkPrioVtPty", "vt_pty_fork_reinit"),
]

INCLUDE_LINE = '#include "src/__support/OSUtil/windows/libc_fork_registry.h"\n'


def add_include(text: str) -> str:
    """Insert the libc_fork_registry.h include after the existing includes,
    if not already present. Inserted in alphabetical position to match
    house style."""
    if "libc_fork_registry.h" in text:
        return text
    # Find the last #include line in the leading include block.
    lines = text.split("\n")
    last_include = -1
    for i, line in enumerate(lines):
        s = line.strip()
        if s.startswith("#include"):
            last_include = i
        elif last_include >= 0 and s and not s.startswith("//") \
                and not s.startswith("#"):
            break
    if last_include < 0:
        return text  # no includes to anchor against; skip
    lines.insert(last_include + 1, INCLUDE_LINE.rstrip("\n"))
    return "\n".join(lines)


def add_registration(text: str, tag: str, prio: str, fn: str) -> str:
    """Append a LIBC_REGISTER_FORK_REINIT(...) line at end of file."""
    marker = f"LIBC_REGISTER_FORK_REINIT({tag},"
    if marker in text:
        return text
    if not text.endswith("\n"):
        text += "\n"
    text += (f"\nLIBC_REGISTER_FORK_REINIT({tag},\n"
             f"                          ::LIBC_NAMESPACE::internal::{prio},\n"
             f"                          &::LIBC_NAMESPACE::internal::{fn})\n")
    return text


def main() -> int:
    missing = []
    for rel, tag, prio, fn in ENTRIES:
        p = ROOT / rel
        if not p.exists():
            missing.append(rel)
            continue
        text = p.read_text(encoding="utf-8")
        text = add_include(text)
        text = add_registration(text, tag, prio, fn)
        p.write_text(text, encoding="utf-8")
        print(f"  ok  {rel} <- {tag} ({prio})")
    if missing:
        print("\nMissing files:", file=sys.stderr)
        for m in missing:
            print(f"  {m}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
