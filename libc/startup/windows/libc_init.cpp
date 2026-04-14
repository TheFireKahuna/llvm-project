//===-- Internal libc initialization for Windows ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// __libc_init() runs inside c.dll where all internal symbols are available.
// Exported so crt_do_start.obj (EXE-side) can call it. Performs allocator
// init, page size queries, TLS setup, argv/environ parsing, and
// subsystem registration.
//
//===----------------------------------------------------------------------===//

#include "config/windows/app.h"
#include "hdr/stdint_proxy.h"
#include "src/__support/OSUtil/windows/ipc/condrv.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/fd/file_pool.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/tls/teb_tls.h"
#include "src/__support/OSUtil/windows/tls/tls_cleanup.h"
#include "src/__support/macros/config.h"
#include "src/__support/process/windows/child_table.h"
#include "src/unistd/environ.h"
#include "src/__support/threads/thread.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/OSUtil/windows/signal/signal.h"
#include "src/stdlib/free.h"
#include "src/stdlib/malloc.h"

// Itanium ABI: run destructors for a DSO (or all if dso == nullptr).
extern "C" void __cxa_finalize(void *dso);

// Runs __cxa_finalize for any DLL that unloads, ensuring Itanium static
// destructors fire even for DLLs without libc's _DllMainCRTStartup.
// Called inside loader lock — must not call LoadLibrary/FreeLibrary.
static void NTAPI dll_unload_callback(ULONG reason,
                                      const LDR_DLL_NOTIFICATION_DATA *data,
                                      PVOID /*context*/) {
  if (reason == LDR_DLL_NOTIFICATION_REASON_UNLOADED)
    __cxa_finalize(data->Unloaded.DllBase);
}

namespace LIBC_NAMESPACE_DECL {

static ThreadAttributes main_thread_attrib;
static TLSDescriptor tls;

static inline bool is_cmdline_space(WCHAR ch) {
  return ch == u' ' || ch == u'\t';
}

static inline bool is_cmdline_argv0_separator(WCHAR ch) {
  return ch == u'\0' || ch <= u' ';
}

struct CommandLineLayout {
  size_t argv_slots = 0; // Includes the trailing nullptr entry.
  size_t wide_units = 0; // Includes terminating NULs for each argument.
};

class CommandLineCountSink {
public:
  CommandLineCountSink() { Layout.argv_slots = 1; }

  void begin_argv0() {}

  void begin_argument() { ++Layout.argv_slots; }

  void emit(WCHAR) { ++Layout.wide_units; }

  void terminate_argument() { ++Layout.wide_units; }

  void finish() { ++Layout.argv_slots; }

  CommandLineLayout layout() const { return Layout; }

private:
  CommandLineLayout Layout;
};

class CommandLineWriteSink {
public:
  CommandLineWriteSink(WCHAR **argv, WCHAR *args) : Argv(argv), Args(args) {
    Layout.argv_slots = 1;
  }

  void begin_argv0() { *Argv++ = Args; }

  void begin_argument() {
    *Argv++ = Args;
    ++Layout.argv_slots;
  }

  void emit(WCHAR ch) {
    *Args++ = ch;
    ++Layout.wide_units;
  }

  void terminate_argument() {
    *Args++ = u'\0';
    ++Layout.wide_units;
  }

  void finish() {
    *Argv = nullptr;
    ++Layout.argv_slots;
  }

  CommandLineLayout layout() const { return Layout; }

private:
  CommandLineLayout Layout;
  WCHAR **Argv;
  WCHAR *Args;
};

// RAII wrapper for malloc/free — replaces the former HeapBlock that used
// RtlAllocateHeap directly. posix_alloc_init() runs before any caller.
class MallocBlock {
public:
  MallocBlock() = default;

  MallocBlock(MallocBlock &&other) noexcept : Block(other.release()) {}

  MallocBlock &operator=(MallocBlock &&other) noexcept {
    if (this == &other)
      return *this;
    reset();
    Block = other.release();
    return *this;
  }

  ~MallocBlock() {
    if (Block)
      LIBC_NAMESPACE::free(Block);
  }

  MallocBlock(const MallocBlock &) = delete;
  MallocBlock &operator=(const MallocBlock &) = delete;

  bool allocate(size_t bytes) {
    reset(LIBC_NAMESPACE::malloc(bytes));
    return Block != nullptr;
  }

  explicit operator bool() const { return Block != nullptr; }

  template <typename T> T *get() const { return static_cast<T *>(Block); }

  template <typename T> T *release_as() {
    return static_cast<T *>(release());
  }

  void reset(void *ptr = nullptr) {
    if (Block)
      LIBC_NAMESPACE::free(Block);
    Block = ptr;
  }

  void *release() {
    void *ptr = Block;
    Block = nullptr;
    return ptr;
  }

private:
  void *Block = nullptr;
};

static bool heap_allocation_size(size_t count, size_t elem_size, size_t extra,
                                 size_t &bytes) {
  size_t product = 0;
  return !__builtin_mul_overflow(count, elem_size, &product) &&
         !__builtin_add_overflow(product, extra, &bytes);
}

static WCHAR *copy_process_string(const UNICODE_STRING &source) {
  size_t wchars = source.Length / sizeof(WCHAR);
  size_t bytes = 0;
  if (!heap_allocation_size(wchars + 1, sizeof(WCHAR), 0, bytes))
    return nullptr;

  auto *copy = static_cast<WCHAR *>(LIBC_NAMESPACE::malloc(bytes));
  if (!copy)
    return nullptr;

  if (source.Length != 0)
    __builtin_memcpy(copy, source.Buffer, source.Length);
  copy[wchars] = u'\0';
  return copy;
}

static const UNICODE_STRING *command_line_source(
    const RTL_USER_PROCESS_PARAMETERS *params) {
  if (!params)
    return nullptr;

  // The decoded kernelbase CommandLineToArgvW substitutes the image path when
  // the caller passes an empty command line. Startup sees the raw PEB strings,
  // so preserve that API contract here instead of exposing the empty buffer.
  if (params->CommandLine.Length != 0 && params->CommandLine.Buffer &&
      params->CommandLine.Buffer[0] != u'\0')
    return &params->CommandLine;

  if (params->ImagePathName.Buffer)
    return &params->ImagePathName;

  return nullptr;
}

class CommandLineParser {
public:
  explicit CommandLineParser(const WCHAR *command_line) : Src(command_line) {}

  template <typename Sink> CommandLineLayout parse(Sink &sink) {
    sink.begin_argv0();
    parse_argv0(sink);
    sink.terminate_argument();

    while (skip_argument_separators()) {
      sink.begin_argument();
      parse_argument(sink);
      sink.terminate_argument();
    }

    sink.finish();
    return sink.layout();
  }

private:
  template <typename Sink> void parse_argv0(Sink &sink) {
    if (*Src == u'"') {
      ++Src;
      while (*Src != u'\0' && *Src != u'"')
        sink.emit(*Src++);
      if (*Src == u'"')
        ++Src;
      return;
    }

    while (!is_cmdline_argv0_separator(*Src))
      sink.emit(*Src++);
    if (*Src != u'\0')
      ++Src;
  }

  bool skip_argument_separators() {
    while (is_cmdline_space(*Src))
      ++Src;
    return *Src != u'\0';
  }

  template <typename Sink> void parse_argument(Sink &sink) {
    while (true) {
      size_t backslashes = consume_backslashes();
      bool copy_current = handle_quote_after_backslashes(backslashes);

      while (backslashes-- > 0)
        sink.emit(u'\\');

      if (*Src == u'\0' || (!InQuotes && is_cmdline_space(*Src)))
        return;

      if (copy_current)
        sink.emit(*Src);
      ++Src;
    }
  }

  size_t consume_backslashes() {
    size_t backslashes = 0;
    while (*Src == u'\\') {
      ++Src;
      ++backslashes;
    }
    return backslashes;
  }

  bool handle_quote_after_backslashes(size_t &backslashes) {
    bool copy_current = true;
    if (*Src != u'"')
      return copy_current;

    if ((backslashes & 1) == 0) {
      copy_current = false;
      if (!InQuotes) {
        InQuotes = true;
      } else {
        InQuotes = false;
        if (Src[1] == u'"') {
          ++Src;
          copy_current = true;
        }
      }
    }

    backslashes /= 2;
    return copy_current;
  }

  const WCHAR *Src;
  bool InQuotes = false;
};

// Convert UTF-16 command line to UTF-8 argv array.
static int build_argv(char ***argv_out) {
  *argv_out = nullptr;

  auto *params = NtCurrentPeb()->ProcessParameters;
  const UNICODE_STRING *source = command_line_source(params);
  if (!source)
    return 0;

  MallocBlock source_copy;
  source_copy.reset(copy_process_string(*source));
  if (!source_copy.get<void>())
    return 0;

  CommandLineParser parser(source_copy.get<WCHAR>());
  CommandLineCountSink counter;
  CommandLineLayout wide_layout = parser.parse(counter);

  size_t wide_argv_bytes = 0;
  size_t wide_storage_bytes = 0;
  size_t wide_block_bytes = 0;
  if (!heap_allocation_size(wide_layout.argv_slots, sizeof(WCHAR *), 0,
                            wide_argv_bytes) ||
      !heap_allocation_size(wide_layout.wide_units, sizeof(WCHAR), 0,
                            wide_storage_bytes) ||
      !heap_allocation_size(1, wide_argv_bytes, wide_storage_bytes,
                            wide_block_bytes)) {
    return 0;
  }

  MallocBlock wide_block;
  if (!wide_block.allocate(wide_block_bytes))
    return 0;

  auto **wargv = wide_block.get<WCHAR *>();
  auto *wargs = reinterpret_cast<WCHAR *>(
      static_cast<unsigned char *>(wide_block.get<void>()) + wide_argv_bytes);
  CommandLineWriteSink writer(wargv, wargs);
  wide_layout = CommandLineParser(source_copy.get<WCHAR>()).parse(writer);

  const size_t argc = wide_layout.argv_slots - 1;
  size_t utf8_bytes = 0;
  for (size_t i = 0; i < argc; ++i) {
    int len = windows::wide_to_utf8_len(wargv[i]);
    if (len <= 0 || __builtin_add_overflow(utf8_bytes, static_cast<size_t>(len),
                                           &utf8_bytes)) {
      return 0;
    }
  }

  size_t argv_bytes = 0;
  size_t argv_block_bytes = 0;
  if (!heap_allocation_size(wide_layout.argv_slots, sizeof(char *), 0,
                            argv_bytes) ||
      !heap_allocation_size(1, argv_bytes, utf8_bytes, argv_block_bytes)) {
    return 0;
  }

  MallocBlock argv_block;
  if (!argv_block.allocate(argv_block_bytes)) {
    return 0;
  }

  auto **argv = argv_block.get<char *>();
  auto *arg_storage = reinterpret_cast<char *>(
      static_cast<unsigned char *>(argv_block.get<void>()) + argv_bytes);

  for (size_t i = 0; i < argc; ++i) {
    int len = windows::wide_to_utf8_len(wargv[i]);
    argv[i] = arg_storage;
    if (windows::wide_to_utf8(wargv[i], arg_storage, len) != len)
      return 0;
    arg_storage += len;
  }
  argv[argc] = nullptr;

  *argv_out = argv_block.release_as<char *>();
  return static_cast<int>(argc);
}

// Build environ from the PEB environment block.
// Returns the pointer array (with ownership bitmap packed after it),
// and writes the entry count to *count_out.
//
// Layout of the returned block (malloc'd — env_grow can free it):
//   [ char*[capacity] | uint8_t[(capacity+7)/8] ]
// where capacity = count + 1 (for the null terminator slot).
//
// String data lives in a single page_alloc arena (not malloc'd).
// Bitmap bits are clear (unowned) so setenv/unsetenv won't free them.
// The arena leaks on clearenv — acceptable for initial environ.
static char **build_environ(int *count_out) {
  *count_out = 0;

  auto *params = NtCurrentPeb()->ProcessParameters;
  WCHAR *env_block =
      params ? static_cast<WCHAR *>(params->Environment) : nullptr;
  if (!env_block)
    return nullptr;

  // Pass 1: count entries.
  int count = 0;
  for (WCHAR *p = env_block; *p; p += windows::wide_string_length(p) + 1)
    ++count;

  if (count == 0)
    return nullptr;

  // Pointer array + ownership bitmap (one malloc — env_grow can free it).
  int capacity = count + 1;
  size_t ptrs_bytes = static_cast<size_t>(capacity) * sizeof(char *);
  size_t bitmap_bytes = (static_cast<size_t>(capacity) + 7) / 8;
  auto **env = static_cast<char **>(
      LIBC_NAMESPACE::malloc(ptrs_bytes + bitmap_bytes));
  if (!env)
    return nullptr;

  // Pass 2: convert each string via individual malloc.
  int i = 0;
  for (WCHAR *p = env_block; *p; p += windows::wide_string_length(p) + 1) {
    int len = windows::wide_to_utf8_len(p);
    if (len <= 0)
      continue;
    char *s = static_cast<char *>(LIBC_NAMESPACE::malloc(len));
    if (!s)
      break;
    windows::wide_to_utf8(p, s, len);
    env[i] = s;
    ++i;
  }
  env[i] = nullptr;
  count = i;

  // Bitmap: all bits set — each string is malloc'd and owned.
  auto *bitmap = reinterpret_cast<uint8_t *>(env + capacity);
  __builtin_memset(bitmap, 0xFF, bitmap_bytes);

  *count_out = count;
  return env;
}

// Set console I/O to UTF-8 and enable VT processing.
// Uses ConDrv IOCTLs directly — no kernel32 console APIs.
static void console_init() {
  auto *params = NtCurrentPeb()->ProcessParameters;
  if (!params || !params->ConsoleHandle)
    return;

  // TODO: implement condrv set_console_cp / get_console_mode / set_console_mode.
  // UTF-8 codepage and VT processing are deferred until condrv IOCTL wrappers
  // are complete.
  (void)params;
}

} // namespace LIBC_NAMESPACE_DECL

// Exported from c.dll so crt_do_start.obj (EXE-side) can call it.
// Performs EXE-specific setup: TLS, argv, environ, signals, child table.
// All subsystem infrastructure is already initialized by __libc_dll_init()
// during c.dll's DLL_PROCESS_ATTACH.
extern "C" __declspec(dllexport) void __libc_init(int *argc_out,
                                                   char ***argv_out,
                                                   char ***env_out) {
  using namespace LIBC_NAMESPACE;

  // posix_alloc_init() and mmap_subsystem_init() are now called by
  // __libc_dll_init() during c.dll DLL_PROCESS_ATTACH (Phase 3).

  init_tls(tls);

  main_thread_attrib.tid = static_cast<int>(NtCurrentThreadId());
  main_thread_attrib.platform_data = &g_pcb.main_thread.lifecycle;
  self.attrib = &main_thread_attrib;
  main_thread_attrib.atexit_callback_mgr =
      internal::get_thread_atexit_callback_mgr();

  char **argv = nullptr;
  int argc = build_argv(&argv);
  g_pcb.startup.argc = argc;
  g_pcb.startup.argv = argv;
  app.argc = argc;     // cross-platform compat
  app.argv = argv;

  int env_count = 0;
  char **env = build_environ(&env_count);
  g_pcb.environment.ptrs = env;
  g_pcb.environment.count = env_count;
  g_pcb.environment.capacity = env_count + 1; // +1 for null terminator slot
  environ = env;                       // wire the POSIX export
  app.env_ptr = env;                   // cross-platform compat

  // Catch DLL unloads to run __cxa_finalize for DSOs that lack their own
  // _DllMainCRTStartup. Safe to double-fire — entries are marked as called.
  PVOID cookie = nullptr;
  if (NT_SUCCESS(LdrRegisterDllNotification(0, dll_unload_callback, nullptr,
                                            &cookie))) {
    g_pcb.veh.dll_notify_cookie = cookie;
    app.dll_notify_cookie = cookie;
  }

  signal_state::init_inherited_child_state();
  process::init_child_table();
  console_init();

  *argc_out = argc;
  *argv_out = argv;
  *env_out = env;
}

// Fork child reinit for the main thread's self.attrib.
//
// After fork, the child gets a new TEB whose loader-managed TLS block is
// zero-initialized from the PE template. This wipes self.attrib (inline
// thread_local). The static main_thread_attrib survives fork via COW
// (it's in .data, not TLS), so we just re-wire self.attrib to it and
// refresh the fields that change across fork (tid, atexit_callback_mgr
// which is a TLS address in the child's fresh TLS block).
void LIBC_NAMESPACE::internal::thread_self_fork_reinit() {
  using namespace LIBC_NAMESPACE;
  main_thread_attrib.tid = static_cast<int>(NtCurrentThreadId());
  main_thread_attrib.platform_data = &g_pcb.main_thread.lifecycle;
  main_thread_attrib.atexit_callback_mgr =
      internal::get_thread_atexit_callback_mgr();
  self.attrib = &main_thread_attrib;
}
