//===--- concurrent_fuzz worker pool impl (NT) -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "test/src/__support/concurrent_fuzz/worker_pool.h"

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/nt/nt_process_types.h"
#include "src/__support/OSUtil/windows/ntdll.h"

#include "hdr/errno_macros.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace concurrent_fuzz {

namespace {

// Per-thread argument passed to the worker entry. Workers stash a
// pointer in the thread-arg (NtCreateThreadEx only carries one void*).
struct WorkerArg {
  uint16_t worker_id;
  WorkerInitFn worker_init;
  SutApplyFn sut_apply;
  void *sut_ctx;
  const Schedule *schedule;
  History *history;
  uint64_t per_worker_tsc_budget;
  uint64_t per_op_tsc_budget;
  cpp::Atomic<uint32_t> *start_gate;     // workers spin-wait until released
  cpp::Atomic<uint32_t> *finished_count; // bumped on exit; for stats
  cpp::Atomic<uint32_t> *timed_out_count;
  cpp::Atomic<uint32_t> *per_op_timeouts;
  // Slowest-op telemetry, written by the worker after slice completion.
  // Read by the aggregator after join, so no atomicity needed.
  uint64_t my_slowest_dt_tsc;
  uint64_t my_slowest_inv_tsc;
  uint16_t my_slowest_kind;
};

// CPU-set ID table populated once at run start.
struct CpuSetTable {
  static constexpr uint32_t kMaxCpuSets = 1024;
  uint32_t ids[kMaxCpuSets];
  uint32_t count;
};

bool probe_cpu_sets(CpuSetTable &out) {
  out.count = 0;
  HANDLE process = NtCurrentProcess();
  ULONG buf_len = 0;
  ::NtQuerySystemInformationEx(SystemCpuSetInformation, &process,
                                sizeof(process), nullptr, 0, &buf_len);
  if (buf_len == 0)
    return false;
  // Stack-allocate; cap the size to avoid blowing the worker thread's
  // stack on pathological systems. Realistic max ~1024 LPs × ~32 B/LP
  // = ~32 KiB; we cap at 32 KiB.
  constexpr ULONG kBufCap = 32u * 1024u;
  if (buf_len > kBufCap)
    return false;
  alignas(8) unsigned char buf[kBufCap];
  NTSTATUS st = ::NtQuerySystemInformationEx(SystemCpuSetInformation, &process,
                                              sizeof(process), buf, buf_len,
                                              &buf_len);
  if (!NT_SUCCESS(st))
    return false;
  ULONG off = 0;
  while (off + sizeof(ULONG) * 2 <= buf_len) {
    auto *e = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION *>(buf + off);
    if (e->Size == 0 || e->Size > buf_len - off)
      break;
    if (e->CpuSet.Id != 0 && out.count < CpuSetTable::kMaxCpuSets) {
      out.ids[out.count++] = e->CpuSet.Id;
    }
    off += e->Size;
  }
  return out.count > 0;
}

bool pin_thread(HANDLE thread, uint32_t cpu_set_id) {
  ULONG id = cpu_set_id;
  NTSTATUS st = ::NtSetInformationThread(thread, ThreadSelectedCpuSets, &id,
                                          sizeof(id));
  return NT_SUCCESS(st);
}

LIBC_MSABI DWORD worker_entry(void *arg) {
  auto *wa = static_cast<WorkerArg *>(arg);

  if (wa->worker_init != nullptr)
    wa->worker_init(wa->sut_ctx);

  // Wait at the start gate so all workers begin within a tight window.
  while (wa->start_gate->load(cpp::MemoryOrder::ACQUIRE) == 0) {
#if defined(__x86_64__) || defined(_M_X64)
    __builtin_ia32_pause();
#endif
  }

  const Schedule &s = *wa->schedule;
  const WorkerSlice &slice = s.slices[wa->worker_id];
  WorkerHistory &ring = wa->history->per_worker[wa->worker_id];

  uint64_t budget = wa->per_worker_tsc_budget;
  uint64_t per_op_budget = wa->per_op_tsc_budget;
  uint64_t deadline = 0;
  if (budget != 0) {
    deadline = rdtsc_lfence_pre() + budget;
  }
  bool timed_out = false;
  bool per_op_violated = false;
  uint64_t worst_dt = 0;
  uint64_t worst_inv = 0;
  uint16_t worst_kind = 0;

  for (uint32_t i = 0; i < slice.count; ++i) {
    const Op &op = s.ops[slice.start + i];

    HistoryEntry e;
    e.op = op;
    e.inv_tsc = rdtsc_lfence_pre();
    e.result = wa->sut_apply(wa->sut_ctx, op);
    e.res_tsc = rdtsc_lfence_post();
    uint64_t dt = e.res_tsc - e.inv_tsc;
    if (dt > worst_dt) {
      worst_dt = dt;
      worst_inv = e.inv_tsc;
      worst_kind = op.kind;
    }
    if (per_op_budget != 0 && dt > per_op_budget) {
      // Rewrite the status so the linearizability checker surfaces the
      // offender instead of a status-zero "all good" entry. The payload
      // is left untouched — it carries whatever the SUT happened to
      // return, which is useful diagnostic context.
      e.result.status = kOpResultStatusTimeoutHint;
      per_op_violated = true;
    }
    (void)ring.push(e);

    if (per_op_violated)
      break;
    if (deadline != 0 && e.res_tsc > deadline) {
      timed_out = true;
      break;
    }
  }

  wa->my_slowest_dt_tsc = worst_dt;
  wa->my_slowest_inv_tsc = worst_inv;
  wa->my_slowest_kind = worst_kind;
  if (timed_out)
    wa->timed_out_count->fetch_add(1, cpp::MemoryOrder::RELAXED);
  if (per_op_violated)
    wa->per_op_timeouts->fetch_add(1, cpp::MemoryOrder::RELAXED);
  wa->finished_count->fetch_add(1, cpp::MemoryOrder::RELEASE);
  return 0;
}

} // namespace

int run_pool(const Schedule &schedule, SutApplyFn sut_apply,
             WorkerInitFn worker_init, void *sut_ctx,
             const PoolParams &params, History &history,
             PoolStats *out_stats) {
  if (sut_apply == nullptr)
    return -EINVAL;
  if (schedule.thread_count == 0 ||
      schedule.thread_count > History::kMaxWorkers)
    return -EINVAL;

  // Probe CPU sets if affinity is requested.
  CpuSetTable cpu_table{};
  bool has_cpu_sets = false;
  if (params.pin_affinity)
    has_cpu_sets = probe_cpu_sets(cpu_table);

  cpp::Atomic<uint32_t> start_gate{0};
  cpp::Atomic<uint32_t> finished_count{0};
  cpp::Atomic<uint32_t> timed_out_count{0};
  cpp::Atomic<uint32_t> per_op_timeouts{0};

  // Per-worker WorkerArg storage (size cap from History).
  WorkerArg wargs[History::kMaxWorkers];
  HANDLE threads[History::kMaxWorkers] = {};
  uint32_t pinned = 0;

  for (uint16_t w = 0; w < schedule.thread_count; ++w) {
    wargs[w].worker_id = w;
    wargs[w].worker_init = worker_init;
    wargs[w].sut_apply = sut_apply;
    wargs[w].sut_ctx = sut_ctx;
    wargs[w].schedule = &schedule;
    wargs[w].history = &history;
    wargs[w].per_worker_tsc_budget = params.per_worker_tsc_budget;
    wargs[w].per_op_tsc_budget = params.per_op_tsc_budget;
    wargs[w].start_gate = &start_gate;
    wargs[w].finished_count = &finished_count;
    wargs[w].timed_out_count = &timed_out_count;
    wargs[w].per_op_timeouts = &per_op_timeouts;
    wargs[w].my_slowest_dt_tsc = 0;
    wargs[w].my_slowest_inv_tsc = 0;
    wargs[w].my_slowest_kind = 0;

    HANDLE t = nullptr;
    NTSTATUS st = ::NtCreateThreadEx(
        &t,
        THREAD_TERMINATE | THREAD_QUERY_INFORMATION |
            THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
        nullptr, NtCurrentProcess(),
        reinterpret_cast<PVOID>(&worker_entry), &wargs[w], 0, 0, 0, 0,
        nullptr);
    if (!NT_SUCCESS(st)) {
      // Bail: terminate any already-spawned workers via TerminateThread,
      // since they're stuck on the start gate.
      for (uint16_t k = 0; k < w; ++k) {
        if (threads[k] != nullptr) {
          ::NtTerminateThread(threads[k], 0);
          ::NtClose(threads[k]);
          threads[k] = nullptr;
        }
      }
      return -EAGAIN;
    }
    threads[w] = t;

    if (has_cpu_sets) {
      uint32_t cpu_set_id = cpu_table.ids[w % cpu_table.count];
      if (pin_thread(t, cpu_set_id))
        ++pinned;
    }
  }

  // Release all workers simultaneously.
  start_gate.store(1, cpp::MemoryOrder::RELEASE);

  // Wait for all workers to complete.
  for (uint16_t w = 0; w < schedule.thread_count; ++w) {
    LARGE_INTEGER timeout;
    timeout.QuadPart = -10000LL * 60000LL; // 60 s safety upper bound
    NTSTATUS st = ::NtWaitForSingleObject(threads[w], FALSE, &timeout);
    if (!NT_SUCCESS(st) || st == STATUS_TIMEOUT) {
      // Worker hung — terminate, mark timed-out, continue draining.
      ::NtTerminateThread(threads[w], 0);
      timed_out_count.fetch_add(1, cpp::MemoryOrder::RELAXED);
    }
    ::NtClose(threads[w]);
  }

  // Merge per-worker rings.
  history.merge();

  if (out_stats != nullptr) {
    out_stats->pinned_workers = pinned;
    out_stats->total_ops_run = history.flat_count;
    out_stats->timed_out_workers =
        timed_out_count.load(cpp::MemoryOrder::ACQUIRE);
    out_stats->per_op_timeouts =
        per_op_timeouts.load(cpp::MemoryOrder::ACQUIRE);
    out_stats->slowest_op_dt_tsc = 0;
    out_stats->slowest_op_inv_tsc = 0;
    out_stats->slowest_op_kind = 0;
    out_stats->slowest_op_worker_id = 0;
    for (uint16_t w = 0; w < schedule.thread_count; ++w) {
      if (wargs[w].my_slowest_dt_tsc > out_stats->slowest_op_dt_tsc) {
        out_stats->slowest_op_dt_tsc = wargs[w].my_slowest_dt_tsc;
        out_stats->slowest_op_inv_tsc = wargs[w].my_slowest_inv_tsc;
        out_stats->slowest_op_kind = wargs[w].my_slowest_kind;
        out_stats->slowest_op_worker_id = w;
      }
    }
  }
  return 0;
}

} // namespace concurrent_fuzz
} // namespace LIBC_NAMESPACE_DECL
