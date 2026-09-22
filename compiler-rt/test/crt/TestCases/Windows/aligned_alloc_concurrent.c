// Exercise the installed allocator, including simultaneous first use. There is
// no allocator call in a constructor in this file; the separate early-use test
// covers that path without accidentally initializing this concurrency test.
// RUN: %clang_crt_main -std=c17 -O2 -mguard=cf -fno-builtin -UNDEBUG -Wall -Wextra -Werror %s -o %t.exe -Wl,/guard:cf
// RUN: %run %t.exe
// REQUIRES: windows, crt

#include "Inputs/aligned_alloc.h"
#include <assert.h>
#include <errno.h>
#include <malloc.h>
#include <stdlib.h>

static HANDLE start;
static volatile LONG stop;
static HANDLE ready, entered;

static DWORD WINAPI blocked_worker(void *unused) {
  (void)unused;
  assert(SetEvent(ready));
  assert(WaitForSingleObject(start, 30000) == WAIT_OBJECT_0);
  assert(SetEvent(entered));
  void *p = aligned_alloc(65536, 37);
  assert(p && (uintptr_t)p % 65536 == 0);
  free(p);
  return 0;
}

static void test_heap_lock(void) {
  assert(ResetEvent(start));
  ready = CreateEventW(NULL, TRUE, FALSE, NULL);
  entered = CreateEventW(NULL, TRUE, FALSE, NULL);
  assert(ready && entered);
  HANDLE worker = CreateThread(NULL, 0, blocked_worker, NULL, 0, NULL);
  assert(worker && WaitForSingleObject(ready, 30000) == WAIT_OBJECT_0);
  assert(HeapLock(GetProcessHeap()));
  // Segment HeapLock starts as a descriptor lock. Walking promotes it to the
  // global allocation barrier; HeapLock alone need not stop other allocators.
  PROCESS_HEAP_ENTRY entry = {0};
  assert(HeapWalk(GetProcessHeap(), &entry));
  // RTL permits the exclusive owner to allocate. Exercise both private page
  // and large paths without self-deadlocking on the global-lock bit.
  void *page = aligned_alloc(65536, 37);
  void *large = aligned_alloc(1 << 20, 37);
  assert(page && large);
  free(page);
  free(large);
  assert(SetEvent(start));
  assert(WaitForSingleObject(entered, 30000) == WAIT_OBJECT_0);
  // The other thread must wait, not return a fabricated ENOMEM.
  assert(WaitForSingleObject(worker, 50) == WAIT_TIMEOUT);
  assert(HeapUnlock(GetProcessHeap()));
  assert(WaitForSingleObject(worker, 30000) == WAIT_OBJECT_0);
  DWORD code;
  assert(GetExitCodeThread(worker, &code) && code == 0);
  assert(CloseHandle(worker));
  assert(CloseHandle(ready));
  assert(CloseHandle(entered));
}

static DWORD WINAPI allocate_worker(void *argument) {
  uintptr_t id = (uintptr_t)argument;
  assert(WaitForSingleObject(start, 30000) == WAIT_OBJECT_0);
  for (unsigned i = 0; i != 2000; ++i) {
    size_t alignment = (size_t)1 << (5 + (i + id) % 22);
    size_t size = i % 3 == 0 ? 1048577 : 97;
    errno = 0;
    unsigned char *p = aligned_alloc(alignment, size);
    // Heap walking must not manufacture allocation failures under contention.
    assert(p);
    assert((uintptr_t)p % alignment == 0);
    assert(HeapSize(GetProcessHeap(), 0, p) >= size);
    p[0] = (unsigned char)id;
    p[size - 1] = (unsigned char)(id + 1);
    if (i % 5 == 0) {
      unsigned char *q = realloc(p, size + 123);
      assert(q);
      p = q;
    }
    assert(p[0] == (unsigned char)id);
    assert(p[size - 1] == (unsigned char)(id + 1));
    free(p);
  }
  return 0;
}

static DWORD WINAPI walk_worker(void *unused) {
  (void)unused;
  assert(WaitForSingleObject(start, 30000) == WAIT_OBJECT_0);
  for (unsigned i = 0; i != 200; ++i) {
    assert(HeapLock(GetProcessHeap()));
    PROCESS_HEAP_ENTRY entry = {0};
    while (HeapWalk(GetProcessHeap(), &entry)) {
    }
    assert(GetLastError() == ERROR_NO_MORE_ITEMS);
    assert(HeapUnlock(GetProcessHeap()));
  }
  InterlockedExchange(&stop, 1);
  return 0;
}

int main(void) {
  if (test_max_alignment() == 16)
    return 0;
  start = CreateEventW(NULL, TRUE, FALSE, NULL);
  assert(start);
  HANDLE threads[9];
  for (uintptr_t i = 0; i != 8; ++i) {
    threads[i] = CreateThread(NULL, 0, allocate_worker, (void *)i, 0, NULL);
    assert(threads[i]);
  }
  threads[8] = CreateThread(NULL, 0, walk_worker, NULL, 0, NULL);
  assert(threads[8]);
  assert(SetEvent(start));
  assert(WaitForMultipleObjects(9, threads, TRUE, 60000) == WAIT_OBJECT_0);
  for (unsigned i = 0; i != 9; ++i) {
    DWORD code;
    assert(GetExitCodeThread(threads[i], &code) && code == 0);
    assert(CloseHandle(threads[i]));
  }
  assert(stop);
  test_heap_lock();
  assert(CloseHandle(start));
  assert(HeapValidate(GetProcessHeap(), 0, NULL));

  // Above the previous prototype envelope. Alignment consumes address space,
  // not committed padding; only 17 bytes of payload need to be writable.
  for (size_t alignment = (size_t)1 << 27; alignment <= ((size_t)1 << 34);
       alignment <<= 1) {
    unsigned char *p = aligned_alloc(alignment, 17);
    assert(p && (uintptr_t)p % alignment == 0);
    p[0] = 0x5a;
    p[16] = 0xa5;
    assert(HeapSize(GetProcessHeap(), 0, p) == 17);
    unsigned char *q = realloc(p, 1048577);
    assert(q && q[0] == 0x5a && q[16] == 0xa5);
    free(q);
  }
  for (unsigned shift = 47; shift != 64; ++shift) {
    errno = 0;
    assert(aligned_alloc((size_t)1 << shift, 17) == NULL);
    assert(errno == ENOMEM);
  }
  return 0;
}
