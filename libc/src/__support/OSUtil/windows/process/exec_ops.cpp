//===-- PID-preserving exec via self-hollowing -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements internal::execve() and internal::execvp().
//
// Self-hollowing exec — replaces the current process image in-place,
// preserving the PID. The calling code lives in c.dll which remains
// mapped throughout the operation. No trampoline assembly needed.
//
// See EXEC_DESIGN.md for the full design rationale and research results.
//
//===----------------------------------------------------------------------===//

#include "exec_ops.h"

#include "hdr/errno_macros.h"
#include "hdr/fcntl_macros.h"
#include "hdr/types/size_t.h"
#include "src/__support/CPP/scope_guard.h"
#include "src/__support/File/file.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/io/env_ops.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/OSUtil/windows/nt/nt_memory_helpers.h"
#include "src/__support/OSUtil/windows/nt/nt_path.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process/process_identity.h"
#include "src/__support/OSUtil/windows/process/process_utils.h"
#include "src/__support/OSUtil/windows/process/shebang.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/macros/config.h"
#include "src/unistd/environ.h"

namespace LIBC_NAMESPACE_DECL {

// =====================================================================
// §1  Internal Types
// =====================================================================

namespace {

// A single IAT patch: write `address` to image_base + `rva`.
struct IatPatch {
  uint32_t rva;
  uintptr_t address;
};

// Maximum threads we can freeze during quiescence.
inline constexpr int MAX_QUIESCE_THREADS = 512;

// Maximum IAT patches (one 4KB page / 12 bytes ≈ 341 entries).
// Typical CRT-linked Win32 target has ~72 imports.
inline constexpr uint32_t MAX_IAT_PATCHES = 340;

// Accumulated state for the self-hollow exec operation.
// All resources are managed explicitly via cleanup().
struct ExecImage {
  HANDLE file = nullptr;
  HANDLE section = nullptr;
  void *temp_base = nullptr;
  SIZE_T temp_view_size = 0;

  SECTION_IMAGE_INFORMATION img_info = {};
  uint32_t entry_rva = 0;
  uint32_t image_size = 0;     // target SizeOfImage
  uint32_t old_image_size = 0; // current EXE SizeOfImage
  uint32_t pdata_rva = 0;
  uint32_t pdata_count = 0;

  IatPatch *patches = nullptr;
  uint32_t patch_count = 0;

  RTL_USER_PROCESS_PARAMETERS *new_params = nullptr;

  // Cleanup all accumulated resources. Safe to call multiple times.
  void cleanup() {
    if (temp_base) {
      ::NtUnmapViewOfSectionEx(NtCurrentProcess(), temp_base, 0);
      temp_base = nullptr;
    }
    if (section) {
      ::NtClose(section);
      section = nullptr;
    }
    if (file) {
      ::NtClose(file);
      file = nullptr;
    }
    if (patches) {
      SIZE_T sz = 0;
      ::NtFreeVirtualMemory(NtCurrentProcess(),
                            reinterpret_cast<void **>(&patches), &sz,
                            MEM_RELEASE);
      patches = nullptr;
    }
    if (new_params) {
      ::RtlDestroyProcessParameters(new_params);
      new_params = nullptr;
    }
    patch_count = 0;
  }
};

// =====================================================================
// §2  PE Validation — Open target, create SEC_IMAGE, validate machine
// =====================================================================

// Returns 0 on success, negative errno on failure.
int exec_open_and_validate(const WCHAR *nt_path, size_t nt_path_len,
                           ExecImage &img) {
  UNICODE_STRING us;
  us.Length = static_cast<USHORT>(nt_path_len * sizeof(WCHAR));
  us.MaximumLength = us.Length + sizeof(WCHAR);
  us.Buffer = const_cast<WCHAR *>(nt_path);

  OBJECT_ATTRIBUTES oa = {};
  oa.Length = sizeof(oa);
  oa.ObjectName = &us;
  oa.Attributes = OBJ_CASE_INSENSITIVE;

  IO_STATUS_BLOCK iosb = {};
  NTSTATUS st = ::NtOpenFile(
      &img.file,
      FILE_EXECUTE | FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE, &oa,
      &iosb, FILE_SHARE_READ | FILE_SHARE_DELETE,
      FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
  if (!NT_SUCCESS(st)) {
    // POSIX: ETXTBSY when the file is open for writing by another process.
    // Our share mode excludes FILE_SHARE_WRITE, so any writer causes
    // STATUS_SHARING_VIOLATION. Map this to ETXTBSY (not generic EBUSY).
    if (st == STATUS_SHARING_VIOLATION)
      return -ETXTBSY;
    return -windows_util::ntstatus_to_errno(st);
  }

  // Create SEC_IMAGE section — the kernel validates the PE signature,
  // checksum, and machine type during section creation.
  st = ::NtCreateSectionEx(&img.section, SECTION_ALL_ACCESS, nullptr, nullptr,
                           PAGE_EXECUTE, SEC_IMAGE, img.file, nullptr, 0);
  if (!NT_SUCCESS(st)) {
    // STATUS_INVALID_IMAGE_FORMAT covers: bad PE, wrong machine in image,
    // 32-bit image on 64-bit, corrupt headers, etc.
    return st == STATUS_INVALID_IMAGE_FORMAT
               ? -ENOEXEC
               : -windows_util::ntstatus_to_errno(st);
  }

  // Query image metadata (entry point, subsystem, machine, stack sizes).
  st = ::NtQuerySection(img.section, SectionImageInformation, &img.img_info,
                        sizeof(img.img_info), nullptr);
  if (!NT_SUCCESS(st))
    return -windows_util::ntstatus_to_errno(st);

  // Validate machine type — self-hollowing requires same architecture.
  if (img.img_info.Machine != IMAGE_FILE_MACHINE_AMD64)
    return -ENOEXEC;

  return 0;
}

// =====================================================================
// §3  Import Resolution — Map temp, walk PE imports, build patch table
// =====================================================================

// Computes the length of a NUL-terminated narrow string.
uint16_t narrow_strlen(const char *s) {
  uint16_t len = 0;
  while (s[len])
    ++len;
  return len;
}

// Returns 0 on success, negative errno on failure.
// On success, img.patches is populated and img.temp_base is still mapped
// (caller must unmap before the image swap).
int exec_resolve_imports(ExecImage &img) {
  // Map at kernel-chosen address for PE parsing.
  NTSTATUS st = ::NtMapViewOfSectionEx(img.section, NtCurrentProcess(),
                                       &img.temp_base, nullptr,
                                       &img.temp_view_size, 0, PAGE_READONLY,
                                       nullptr, 0);
  if (!NT_SUCCESS(st))
    return -windows_util::ntstatus_to_errno(st);

  auto *base = static_cast<uint8_t *>(img.temp_base);

  // Parse PE headers.
  auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return -ENOEXEC;

  auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE)
    return -ENOEXEC;

  img.entry_rva = nt->OptionalHeader.AddressOfEntryPoint;
  img.image_size = nt->OptionalHeader.SizeOfImage;

  // Extract .pdata (exception directory) info for RtlAddFunctionTable.
  const auto &exc_dir =
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
  if (exc_dir.VirtualAddress && exc_dir.Size >= 12) {
    img.pdata_rva = exc_dir.VirtualAddress;
    // Each RUNTIME_FUNCTION is 12 bytes (BeginAddress, EndAddress, UnwindData).
    img.pdata_count = exc_dir.Size / 12;
  }

  // Allocate IAT patch table — one page, private memory (survives image swap).
  SIZE_T patch_alloc = sizeof(IatPatch) * MAX_IAT_PATCHES;
  patch_alloc = (patch_alloc + 4095) & ~SIZE_T{4095}; // round to page
  void *patch_mem = nullptr;
  st = ::NtAllocateVirtualMemoryEx(NtCurrentProcess(), &patch_mem, &patch_alloc,
                                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE,
                                   nullptr, 0);
  if (!NT_SUCCESS(st))
    return -ENOMEM;
  img.patches = static_cast<IatPatch *>(patch_mem);

  // Walk import table — each IMAGE_IMPORT_DESCRIPTOR is one imported DLL.
  const auto &imp_dd =
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!imp_dd.VirtualAddress || imp_dd.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR))
    return 0; // No imports — valid for static binaries.

  auto *imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR *>(
      base + imp_dd.VirtualAddress);

  for (; imp->Name; ++imp) {
    const char *dll_name =
        reinterpret_cast<const char *>(base + imp->Name);

    // Convert ASCII DLL name to UNICODE_STRING for LdrLoadDll.
    WCHAR dll_wide[256];
    int i = 0;
    for (; dll_name[i] && i < 255; ++i)
      dll_wide[i] = static_cast<WCHAR>(static_cast<unsigned char>(dll_name[i]));
    dll_wide[i] = L'\0';

    UNICODE_STRING dll_ustr;
    dll_ustr.Length = static_cast<USHORT>(i * sizeof(WCHAR));
    dll_ustr.MaximumLength = dll_ustr.Length + sizeof(WCHAR);
    dll_ustr.Buffer = dll_wide;

    // LdrLoadDll is a no-op for already-loaded DLLs (returns existing base).
    // For new DLLs, it loads them correctly (DllMain, dependency chains, etc.).
    void *dll_base = nullptr;
    st = ::LdrLoadDll(nullptr, nullptr, &dll_ustr, &dll_base);
    if (!NT_SUCCESS(st))
      return -ENOEXEC; // Required DLL cannot be loaded.

    // Walk the Import Lookup Table (ILT) and build patches for the IAT.
    // ILT entries describe what to import; IAT entries are where to write.
    uint32_t ilt_rva = imp->OriginalFirstThunk ? imp->OriginalFirstThunk
                                               : imp->FirstThunk;
    const auto *ilt = reinterpret_cast<const uint64_t *>(base + ilt_rva);
    uint32_t iat_rva = imp->FirstThunk;

    for (uint32_t idx = 0; ilt[idx]; ++idx) {
      void *resolved = nullptr;
      uint64_t entry = ilt[idx];

      if (entry & (1ULL << 63)) {
        // Ordinal import — bits [15:0] are the ordinal number.
        ULONG ordinal = static_cast<ULONG>(entry & 0xFFFF);
        st = ::LdrGetProcedureAddress(dll_base, nullptr, ordinal, &resolved);
      } else {
        // Named import — entry is RVA to IMAGE_IMPORT_BY_NAME.
        // [0..1] = hint (unused here), [2..] = NUL-terminated ASCII name.
        auto *by_name = base + (entry & 0x7FFFFFFF);
        const char *func_name = reinterpret_cast<const char *>(by_name + 2);

        STRING ansi;
        ansi.Buffer = const_cast<char *>(func_name);
        ansi.Length = narrow_strlen(func_name);
        ansi.MaximumLength = ansi.Length + 1;

        st = ::LdrGetProcedureAddress(dll_base, &ansi, 0, &resolved);
      }

      if (!NT_SUCCESS(st))
        return -ENOEXEC; // Required function not found.

      if (img.patch_count >= MAX_IAT_PATCHES)
        return -E2BIG; // Pathological number of imports.

      img.patches[img.patch_count].rva = iat_rva + idx * 8;
      img.patches[img.patch_count].address =
          reinterpret_cast<uintptr_t>(resolved);
      ++img.patch_count;
    }
  }

  return 0;
}

// =====================================================================
// §4  LDR/PEB State Update
// =====================================================================

// Find the EXE's LDR_DATA_TABLE_ENTRY (first entry in InLoadOrderModuleList).
// The EXE is always the first module in load order.
LDR_DATA_TABLE_ENTRY *find_exe_ldr_entry() {
  PEB *peb = NtCurrentPeb();
  auto *first = peb->Ldr->InLoadOrderModuleList.Flink;
  // InLoadOrderLinks is at offset 0 of LDR_DATA_TABLE_ENTRY.
  return reinterpret_cast<LDR_DATA_TABLE_ENTRY *>(first);
}

// Update LDR entry and PEB for the new image.
// Same-base strategy: DllBase is unchanged, so RB trees need no surgery.
// Returns 0 on success, negative errno on failure.
int exec_update_process_state(ExecImage &img, const WCHAR *image_path_win32,
                              size_t image_path_chars, char *const argv[],
                              char *const envp[]) {
  PEB *peb = NtCurrentPeb();
  auto *old_base = static_cast<uint8_t *>(peb->ImageBaseAddress);

  // --- Update LDR entry fields (DllBase unchanged — same-base strategy) ---
  LDR_DATA_TABLE_ENTRY *ldr = find_exe_ldr_entry();
  img.old_image_size = ldr->SizeOfImage;

  ldr->SizeOfImage = img.image_size;
  ldr->EntryPoint =
      reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(old_base) +
                               img.entry_rva);
  // OriginalBase: the target PE's preferred ImageBase (for relocation info).
  if (img.temp_base) {
    auto *dos =
        reinterpret_cast<const IMAGE_DOS_HEADER *>(img.temp_base);
    auto *nt_hdr = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(
        static_cast<const uint8_t *>(img.temp_base) + dos->e_lfanew);
    ldr->OriginalBase =
        reinterpret_cast<PVOID>(nt_hdr->OptionalHeader.ImageBase);
    ldr->TimeDateStamp = nt_hdr->FileHeader.TimeDateStamp;
  }

  // --- Create new ProcessParameters ---
  using process_utils::build_cmdline;
  using process_utils::build_env_block;
  using process_utils::utf8_to_wide;
  using process_utils::utf8_to_wide_len;

  UNICODE_STRING new_image_path;
  new_image_path.Length =
      static_cast<USHORT>(image_path_chars * sizeof(WCHAR));
  new_image_path.MaximumLength = new_image_path.Length + sizeof(WCHAR);
  new_image_path.Buffer = const_cast<WCHAR *>(image_path_win32);

  // Build wide command line from argv.
  SIZE_T cmdline_bytes = build_cmdline(argv, nullptr);
  WCHAR *wcmdline = nullptr;
  WCHAR cmdline_fallback[2] = {L'\0', L'\0'};
  // Stack-allocate a scratch buffer for reasonable command lines.
  // For pathological sizes, use the image path as a fallback.
  WCHAR cmdline_buf[4096];
  if (cmdline_bytes > 0 && cmdline_bytes <= sizeof(cmdline_buf)) {
    wcmdline = cmdline_buf;
    build_cmdline(argv, wcmdline);
  } else if (cmdline_bytes > sizeof(cmdline_buf)) {
    // Allocate from process heap for large command lines.
    wcmdline = static_cast<WCHAR *>(::RtlAllocateHeap(
        peb->ProcessHeap, 0, cmdline_bytes));
    if (wcmdline)
      build_cmdline(argv, wcmdline);
    else
      wcmdline = cmdline_fallback;
  } else {
    wcmdline = cmdline_fallback;
  }

  UNICODE_STRING new_cmdline;
  SIZE_T cmdline_wchars =
      cmdline_bytes > 0 ? cmdline_bytes / sizeof(WCHAR) - 1 : 0;
  new_cmdline.Length = static_cast<USHORT>(cmdline_wchars * sizeof(WCHAR));
  new_cmdline.MaximumLength = new_cmdline.Length + sizeof(WCHAR);
  new_cmdline.Buffer = wcmdline;

  // Build wide environment block from envp (or inherit current).
  PVOID new_env = nullptr;
  if (envp) {
    SIZE_T env_bytes = build_env_block(envp, nullptr);
    if (env_bytes > 0) {
      new_env = ::RtlAllocateHeap(peb->ProcessHeap, 0, env_bytes);
      if (new_env)
        build_env_block(envp, static_cast<WCHAR *>(new_env));
    }
  }

  RTL_USER_PROCESS_PARAMETERS *old_params = peb->ProcessParameters;

  NTSTATUS st = ::RtlCreateProcessParametersEx(
      &img.new_params, &new_image_path,
      &old_params->DllPath,                    // inherit DLL search path
      &old_params->CurrentDirectory.DosPath,   // inherit cwd
      &new_cmdline,                            // new command line
      new_env,                                 // new env or NULL to inherit
      &old_params->WindowTitle,                // inherit
      &old_params->DesktopInfo,                // inherit
      nullptr,                                 // ShellInfo — clear
      nullptr,                                 // RuntimeData — clear
      RTL_USER_PROC_PARAMS_NORMALIZED);

  // Free heap-allocated command line if we used it.
  if (wcmdline != cmdline_buf && wcmdline != cmdline_fallback && wcmdline)
    ::RtlFreeHeap(peb->ProcessHeap, 0, wcmdline);

  if (!NT_SUCCESS(st))
    return -ENOMEM;

  // Copy fields not set by RtlCreateProcessParametersEx.
  img.new_params->ConsoleHandle = old_params->ConsoleHandle;
  img.new_params->ConsoleFlags = old_params->ConsoleFlags;
  img.new_params->StandardInput = old_params->StandardInput;
  img.new_params->StandardOutput = old_params->StandardOutput;
  img.new_params->StandardError = old_params->StandardError;
  img.new_params->CurrentDirectory.Handle =
      old_params->CurrentDirectory.Handle;
  img.new_params->WindowFlags = old_params->WindowFlags;
  img.new_params->ShowWindowFlags = old_params->ShowWindowFlags;

  // Swap PEB pointer (threads are not yet quiesced, but ProcessParameters
  // updates are a single pointer-width store — naturally atomic on x64).
  peb->ProcessParameters = img.new_params;
  img.new_params = nullptr; // PEB owns it now; don't free in cleanup().

  // Free old parameters.
  ::RtlDestroyProcessParameters(old_params);

  return 0;
}

// =====================================================================
// §5  Pre-Swap Teardown
// =====================================================================

// Flush all buffered FILE* streams to prevent data loss.
void exec_flush_streams() {
  LIBC_NAMESPACE::File::lock_list();
  for (auto *f = LIBC_NAMESPACE::File::get_first_file(); f;
       f = f->get_next()) {
    if (f->try_lock_for_flush()) {
      f->flush_unlocked();
      f->unlock();
    }
  }
  LIBC_NAMESPACE::File::unlock_list();
}

// Close all FD_CLOEXEC file descriptors (POSIX: exec closes them).
void exec_close_cloexec_fds() {
  int hw = internal::fd_table.high_water();
  for (int fd = 0; fd < hw; ++fd) {
    auto *slot = internal::fd_table.get_slot(fd);
    if (!slot)
      continue;
    if (slot->cloexec(cpp::MemoryOrder::RELAXED))
      internal::fd_table.release(fd);
  }
}

// Deallocate stale FLS data without invoking callbacks. After self-hollow,
// FLS callback pointers reference the old (unmapped) image — invoking them
// would crash. NULLing TEB->FlsData ensures LdrShutdownProcess skips FLS
// processing if the target later calls ExitProcess.
void exec_cleanup_fls() {
  void *fls_data = NtCurrentTeb()->FlsData;
  if (fls_data) {
    ::RtlProcessFlsData(fls_data, RTL_FLS_DATA_CLEANUP_DEALLOCATE);
    NtCurrentTeb()->FlsData = nullptr;
  }
}

// Freeze all threads except the current one. State-change handles are
// intentionally leaked — frozen threads stay frozen until NtTerminateProcess
// kills the entire process from the target entry point.
// Returns 0 on success, negative errno on failure.
int exec_quiesce_threads() {
  HANDLE self = NtCurrentTeb()->ClientId.UniqueThread;
  HANDLE prev = nullptr;
  int count = 0;

  while (count < MAX_QUIESCE_THREADS) {
    HANDLE next = nullptr;
    NTSTATUS st = ::NtGetNextThread(NtCurrentProcess(), prev,
                                    THREAD_ALL_ACCESS, 0, 0, &next);
    if (prev)
      ::NtClose(prev);
    if (!NT_SUCCESS(st))
      break; // No more threads.

    // Skip self.
    THREAD_BASIC_INFORMATION tbi = {};
    st = ::NtQueryInformationThread(next, ThreadBasicInformation, &tbi,
                                    sizeof(tbi), nullptr);
    if (NT_SUCCESS(st) && tbi.ClientId.UniqueThread == self) {
      prev = next;
      continue;
    }

    // Create per-thread state-change handle and suspend.
    HANDLE sc = nullptr;
    OBJECT_ATTRIBUTES sc_oa = {};
    sc_oa.Length = sizeof(sc_oa);
    st = ::NtCreateThreadStateChange(&sc, THREAD_ALL_ACCESS, &sc_oa, next, 0);
    if (NT_SUCCESS(st)) {
      ::NtChangeThreadState(sc, next, ThreadStateSuspend, nullptr, 0, 0);
      // Intentionally leak sc — keeps thread frozen until process termination.
    }

    prev = next;
    ++count;
  }
  if (prev)
    ::NtClose(prev);

  return 0;
}

// =====================================================================
// §5a  TLS Directory Handling (Win32 target support)
// =====================================================================

// Clean up the old image's TLS slot. Clears the bitmap bit and NULLs
// the TEB entry so stale TLS data doesn't leak across exec.
void exec_teardown_old_tls(uint8_t *old_base) {
  auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(old_base);
  auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(old_base +
                                                           dos->e_lfanew);
  const auto &tls_dd =
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
  if (!tls_dd.VirtualAddress || tls_dd.Size < sizeof(IMAGE_TLS_DIRECTORY64))
    return;

  auto *tls_dir = reinterpret_cast<const IMAGE_TLS_DIRECTORY64 *>(
      old_base + tls_dd.VirtualAddress);
  uint32_t old_index =
      *reinterpret_cast<const uint32_t *>(tls_dir->AddressOfIndex);

  // Clear the bit in PEB->TlsBitmap.
  PEB *peb = NtCurrentPeb();
  if (peb->TlsBitmap && old_index < peb->TlsBitmap->SizeOfBitMap) {
    ULONG word_idx = old_index / 32;
    ULONG bit_mask = 1u << (old_index % 32);
    __atomic_fetch_and(&peb->TlsBitmap->Buffer[word_idx], ~bit_mask,
                       __ATOMIC_RELEASE);
  }

  // NULL the TEB slot for the old index.
  void **tls_array;
  __asm__ volatile("movq %%gs:0x58, %0" : "=r"(tls_array));
  if (tls_array && old_index < TLS_MINIMUM_AVAILABLE)
    tls_array[old_index] = nullptr;
}

// Allocate a TLS slot from PEB->TlsBitmap using CAS (same lock-free
// pattern as tls_alloc() in teb_tls.h). Returns the slot index, or
// ~0u on failure.
uint32_t exec_alloc_tls_slot() {
  PEB *peb = NtCurrentPeb();
  if (!peb->TlsBitmap)
    return ~0u;

  ULONG total_bits = peb->TlsBitmap->SizeOfBitMap;
  ULONG num_words = (total_bits + 31) / 32;

  for (ULONG w = 0; w < num_words; ++w) {
    ULONG val = __atomic_load_n(&peb->TlsBitmap->Buffer[w],
                                __ATOMIC_ACQUIRE);
    while (val != ~0u) {
      ULONG bit = __builtin_ctz(~val);
      ULONG new_val = val | (1u << bit);
      if (__atomic_compare_exchange_n(&peb->TlsBitmap->Buffer[w], &val,
                                      new_val, /*weak=*/true,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return w * 32 + bit;
    }
  }
  return ~0u;
}

// Initialize TLS for the new image after self-hollow remap.
// Allocates a slot, copies static initializers, installs in TEB,
// and fires TLS callbacks. Must be called AFTER the image is mapped
// at its final address and IAT patches are applied.
void exec_init_tls(uint8_t *new_base) {
  auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(new_base);
  auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(new_base +
                                                           dos->e_lfanew);
  const auto &tls_dd =
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
  if (!tls_dd.VirtualAddress || tls_dd.Size < sizeof(IMAGE_TLS_DIRECTORY64))
    return;

  auto *tls_dir = reinterpret_cast<IMAGE_TLS_DIRECTORY64 *>(
      new_base + tls_dd.VirtualAddress);

  size_t raw_size = tls_dir->EndAddressOfRawData -
                    tls_dir->StartAddressOfRawData;
  size_t total_size = raw_size + tls_dir->SizeOfZeroFill;
  bool has_tls_data = (total_size > 0);

  uint32_t new_index = exec_alloc_tls_slot();
  if (new_index == ~0u)
    return;

  // Write slot index into the image's _tls_index variable.
  // This faults in a CoW page from the SEC_IMAGE mapping.
  void *index_addr = reinterpret_cast<void *>(tls_dir->AddressOfIndex);
  void *index_page = reinterpret_cast<void *>(
      reinterpret_cast<uintptr_t>(index_addr) & ~uintptr_t{4095});
  SIZE_T page_size = 4096;
  ULONG old_prot = 0;
  ::NtProtectVirtualMemory(NtCurrentProcess(), &index_page, &page_size,
                           PAGE_READWRITE, &old_prot);
  *reinterpret_cast<uint32_t *>(index_addr) = new_index;
  ::NtProtectVirtualMemory(NtCurrentProcess(), &index_page, &page_size,
                           old_prot, &old_prot);

  // Update LDR entry TlsIndex.
  LDR_DATA_TABLE_ENTRY *ldr = find_exe_ldr_entry();
  ldr->TlsIndex = static_cast<USHORT>(new_index);

  // Allocate per-thread TLS data block.
  void *tls_block = nullptr;
  if (has_tls_data) {
    PEB *peb = NtCurrentPeb();
    tls_block = ::RtlAllocateHeap(peb->ProcessHeap, 0, total_size);
    if (!tls_block)
      return;

    __builtin_memcpy(tls_block,
                     reinterpret_cast<void *>(tls_dir->StartAddressOfRawData),
                     raw_size);
    if (tls_dir->SizeOfZeroFill > 0)
      __builtin_memset(static_cast<char *>(tls_block) + raw_size, 0,
                       tls_dir->SizeOfZeroFill);
  }

  // Install in TEB ThreadLocalStoragePointer array.
  void **tls_array;
  __asm__ volatile("movq %%gs:0x58, %0" : "=r"(tls_array));
  if (tls_array && new_index < TLS_MINIMUM_AVAILABLE)
    tls_array[new_index] = tls_block;

  // Fire TLS callbacks with DLL_PROCESS_ATTACH.
  if (tls_dir->AddressOfCallBacks) {
    auto *callbacks = reinterpret_cast<PIMAGE_TLS_CALLBACK *>(
        tls_dir->AddressOfCallBacks);
    for (; callbacks && *callbacks; ++callbacks)
      (*callbacks)(reinterpret_cast<void *>(new_base), DLL_PROCESS_ATTACH,
                   nullptr);
  }
}

// =====================================================================
// §5b  CFG Bitmap Registration (Win32 target support)
// =====================================================================

// Register all CFG call targets from the target image's guard function
// table. Without this, indirect calls in CFG-enabled targets would fault.
void exec_register_cfg_targets(uint8_t *new_base) {
  auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(new_base);
  auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(new_base +
                                                           dos->e_lfanew);

  if (!(nt->OptionalHeader.DllCharacteristics &
        IMAGE_DLLCHARACTERISTICS_GUARD_CF))
    return;

  const auto &lc_dd =
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
  if (!lc_dd.VirtualAddress || lc_dd.Size < 0x94)
    return;

  auto *load_config = reinterpret_cast<const IMAGE_LOAD_CONFIG_DIRECTORY64 *>(
      new_base + lc_dd.VirtualAddress);

  if (!(load_config->GuardFlags & IMAGE_GUARD_CF_FUNCTION_TABLE_PRESENT))
    return;

  uint64_t func_count = load_config->GuardCFFunctionCount;
  if (func_count == 0)
    return;

  // GuardFlags bits [28:20] encode additional bytes per entry beyond the
  // 4-byte RVA (e.g., export suppression metadata).
  uint32_t entry_extra_bytes = (load_config->GuardFlags >> 20) & 0xF;
  uint32_t entry_stride = 4 + entry_extra_bytes;

  // GuardCFFunctionTable is a VA (kernel-relocated after remap).
  auto *func_table = reinterpret_cast<const uint8_t *>(
      load_config->GuardCFFunctionTable);

  auto base_addr = reinterpret_cast<uintptr_t>(new_base);

  // Batch CFG registration by page for efficiency.
  constexpr uint32_t BATCH_SIZE = 256;
  CFG_CALL_TARGET_INFO batch[BATCH_SIZE];
  uintptr_t current_page = 0;
  uint32_t batch_count = 0;

  auto flush_batch = [&]() {
    if (batch_count == 0)
      return;
    ULONG processed = 0;
    CFG_CALL_TARGET_LIST_INFORMATION info = {};
    info.NumberOfEntries = batch_count;
    info.NumberOfEntriesProcessed = &processed;
    info.CallTargetInfo = batch;

    MEMORY_RANGE_ENTRY range = {};
    range.VirtualAddress = reinterpret_cast<PVOID>(current_page);
    range.NumberOfBytes = 0x1000;

    ::NtSetInformationVirtualMemory(NtCurrentProcess(),
                                    VmCfgCallTargetInformation, 1, &range,
                                    &info, sizeof(info));
    batch_count = 0;
  };

  for (uint64_t i = 0; i < func_count; ++i) {
    uint32_t rva;
    __builtin_memcpy(&rva, func_table + i * entry_stride, 4);

    uintptr_t target_addr = base_addr + rva;
    uintptr_t page = target_addr & ~uintptr_t{0xFFF};
    uintptr_t offset = target_addr & 0xFFF;

    if (page != current_page && batch_count > 0)
      flush_batch();

    current_page = page;
    batch[batch_count].Offset = offset;
    batch[batch_count].Flags = CFG_CALL_TARGET_VALID;
    ++batch_count;

    if (batch_count >= BATCH_SIZE)
      flush_batch();
  }
  flush_batch();

  // Register entry point as a valid call target.
  nt_helpers::cfg_register_target(reinterpret_cast<void *>(
      base_addr + nt->OptionalHeader.AddressOfEntryPoint));

  // NOTE: GuardCFCheckFunctionPointer / GuardCFDispatchFunctionPointer
  // patching requires finding private ntdll symbols
  // (LdrpValidateUserCallTarget / LdrpDispatchUserCallTarget).
  // The CRT's crt_cfg.obj initializes these to no-op stubs that pass
  // all checks. Bitmap registration above is sufficient for correctness.
  // Dispatch pointer patching can be added later via ntdll code scanning.
}

// =====================================================================
// §5c  LDR Name Update (Win32 target support)
// =====================================================================

// LdrpHashTable bucket count — fixed at 32 in all NT versions.
inline constexpr uint32_t LDRP_HASH_TABLE_SIZE = 32;

// Find LdrpHashTable by walking the hash chain from the EXE's
// HashLinks to find a LIST_ENTRY inside ntdll's address range.
LIST_ENTRY *find_ldrp_hash_table(LDR_DATA_TABLE_ENTRY *ldr,
                                 uint32_t expected_bucket) {
  // ntdll is the second module in InLoadOrderModuleList.
  auto *ntdll_entry = reinterpret_cast<LDR_DATA_TABLE_ENTRY *>(
      ldr->InLoadOrderLinks.Flink);
  auto ntdll_start = reinterpret_cast<uintptr_t>(ntdll_entry->DllBase);
  auto ntdll_end = ntdll_start + ntdll_entry->SizeOfImage;

  // Walk the doubly-linked hash chain. One entry is the bucket head
  // (inside ntdll's .data section).
  LIST_ENTRY *cur = ldr->HashLinks.Flink;
  while (cur != &ldr->HashLinks) {
    auto addr = reinterpret_cast<uintptr_t>(cur);
    if (addr >= ntdll_start && addr < ntdll_end) {
      // Found the bucket head. Compute table base.
      return reinterpret_cast<LIST_ENTRY *>(
          addr - expected_bucket * sizeof(LIST_ENTRY));
    }
    cur = cur->Flink;
  }
  return nullptr;
}

// Update a UNICODE_STRING's buffer. In-place if it fits, else
// ProcessHeap alloc (old buffer on loader heap leaks — acceptable).
void update_unicode_string(UNICODE_STRING &ustr, const WCHAR *new_str,
                           USHORT new_byte_len) {
  USHORT alloc_needed = new_byte_len + sizeof(WCHAR);

  if (alloc_needed <= ustr.MaximumLength) {
    __builtin_memcpy(ustr.Buffer, new_str, new_byte_len);
    ustr.Buffer[new_byte_len / sizeof(WCHAR)] = L'\0';
    ustr.Length = new_byte_len;
  } else {
    PEB *peb = NtCurrentPeb();
    auto *buf = static_cast<WCHAR *>(
        ::RtlAllocateHeap(peb->ProcessHeap, 0, alloc_needed));
    if (!buf)
      return;
    __builtin_memcpy(buf, new_str, new_byte_len);
    buf[new_byte_len / sizeof(WCHAR)] = L'\0';
    ustr.Buffer = buf;
    ustr.Length = new_byte_len;
    ustr.MaximumLength = alloc_needed;
  }
}

// Full LDR name update: FullDllName, BaseDllName, hash, and bucket relink.
void exec_update_ldr_names(LDR_DATA_TABLE_ENTRY *ldr,
                           const WCHAR *full_path, size_t full_path_chars) {
  // Extract base name (filename after last separator).
  const WCHAR *base_name = full_path;
  for (size_t i = 0; i < full_path_chars; ++i) {
    if (full_path[i] == L'\\' || full_path[i] == L'/')
      base_name = full_path + i + 1;
  }
  size_t base_name_chars = full_path_chars - (base_name - full_path);

  auto full_byte_len = static_cast<USHORT>(full_path_chars * sizeof(WCHAR));
  auto base_byte_len = static_cast<USHORT>(base_name_chars * sizeof(WCHAR));

  // Tier 1: Always update FullDllName (needed for dladdr).
  update_unicode_string(ldr->FullDllName, full_path, full_byte_len);

  // Tier 2: Update BaseDllName + hash table surgery.
  ULONG old_hash = ldr->BaseNameHashValue;
  uint32_t old_bucket = old_hash % LDRP_HASH_TABLE_SIZE;

  LIST_ENTRY *hash_table = find_ldrp_hash_table(ldr, old_bucket);

  update_unicode_string(ldr->BaseDllName, base_name, base_byte_len);

  ULONG new_hash = 0;
  ::RtlHashUnicodeString(&ldr->BaseDllName, TRUE,
                         HASH_STRING_ALGORITHM_DEFAULT, &new_hash);
  ldr->BaseNameHashValue = new_hash;

  uint32_t new_bucket = new_hash % LDRP_HASH_TABLE_SIZE;

  if (hash_table && old_bucket != new_bucket) {
    // Unlink from old bucket.
    ldr->HashLinks.Blink->Flink = ldr->HashLinks.Flink;
    ldr->HashLinks.Flink->Blink = ldr->HashLinks.Blink;

    // Insert at head of new bucket.
    LIST_ENTRY *new_head = &hash_table[new_bucket];
    ldr->HashLinks.Flink = new_head->Flink;
    ldr->HashLinks.Blink = new_head;
    new_head->Flink->Blink = &ldr->HashLinks;
    new_head->Flink = &ldr->HashLinks;
  }
  // If hash_table is nullptr, names and hash are still updated —
  // dladdr/GetModuleFileNameW correct, GetModuleHandleW stale.
}

// =====================================================================
// §6  Image Swap — The Point of No Return
// =====================================================================

// Clear VA range [old_end, new_end) if target is larger than old image.
// All non-current threads must already be quiesced.
// Returns 0 on success, negative errno on failure.
int exec_clear_extra_va(uintptr_t old_base, uint32_t old_size,
                        uint32_t new_size) {
  if (new_size <= old_size)
    return 0;

  uintptr_t scan_addr = old_base + old_size;
  uintptr_t scan_end = old_base + new_size;

  while (scan_addr < scan_end) {
    MEMORY_BASIC_INFORMATION mbi = {};
    NTSTATUS st = ::NtQueryVirtualMemory(
        NtCurrentProcess(), reinterpret_cast<void *>(scan_addr),
        MemoryBasicInformation, &mbi, sizeof(mbi), nullptr);
    if (!NT_SUCCESS(st))
      return -ENOMEM;

    uintptr_t region_end =
        reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (region_end > scan_end)
      region_end = scan_end;

    if (mbi.State != MEM_FREE) {
      if (mbi.Type == MEM_PRIVATE) {
        void *addr = mbi.AllocationBase;
        SIZE_T sz = 0;
        ::NtFreeVirtualMemory(NtCurrentProcess(), &addr, &sz, MEM_RELEASE);
      } else {
        // MEM_MAPPED or MEM_IMAGE — unmap the view.
        ::NtUnmapViewOfSectionEx(NtCurrentProcess(), mbi.AllocationBase, 0);
      }
    }

    scan_addr = region_end;
  }
  return 0;
}

// Apply IAT patches to the remapped image.
void exec_apply_iat_patches(uint8_t *base, const IatPatch *patches,
                            uint32_t count) {
  if (!count)
    return;

  // Determine the byte range of all patches for protection change.
  uint32_t min_rva = patches[0].rva;
  uint32_t max_rva = patches[0].rva;
  for (uint32_t i = 1; i < count; ++i) {
    if (patches[i].rva < min_rva)
      min_rva = patches[i].rva;
    if (patches[i].rva > max_rva)
      max_rva = patches[i].rva;
  }

  // Make the IAT range writable.
  void *protect_addr =
      reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(base + min_rva) &
                               ~uintptr_t{4095});
  SIZE_T protect_size =
      (base + max_rva + 8) -
      static_cast<uint8_t *>(protect_addr);
  protect_size = (protect_size + 4095) & ~SIZE_T{4095};
  ULONG old_prot = 0;
  ::NtProtectVirtualMemory(NtCurrentProcess(), &protect_addr, &protect_size,
                           PAGE_READWRITE, &old_prot);

  // Write resolved addresses.
  for (uint32_t i = 0; i < count; ++i) {
    auto *slot = reinterpret_cast<uintptr_t *>(base + patches[i].rva);
    *slot = patches[i].address;
  }

  // Restore protection.
  ::NtProtectVirtualMemory(NtCurrentProcess(), &protect_addr, &protect_size,
                           old_prot, &old_prot);
}

// Allocate a new stack for the target entry point.
// Returns pointer to the TOP of the stack (highest address), or nullptr.
void *exec_allocate_stack(SIZE_T reserve_size, SIZE_T commit_size) {
  if (reserve_size < 65536)
    reserve_size = 65536;
  if (commit_size < 8192)
    commit_size = 8192;
  if (commit_size > reserve_size)
    commit_size = reserve_size;

  // Reserve full stack VA.
  void *stack_base = nullptr;
  SIZE_T alloc_size = reserve_size;
  NTSTATUS st = ::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &stack_base, &alloc_size,
      MEM_RESERVE, PAGE_READWRITE, nullptr, 0);
  if (!NT_SUCCESS(st))
    return nullptr;

  // Commit pages from the top down (stack grows downward).
  // Leave the lowest page uncommitted (hard guard).
  auto *commit_start =
      static_cast<uint8_t *>(stack_base) + reserve_size - commit_size;
  if (commit_start <
      static_cast<uint8_t *>(stack_base) + 4096)
    commit_start = static_cast<uint8_t *>(stack_base) + 4096;

  void *commit_addr = commit_start;
  SIZE_T commit_len = static_cast<uint8_t *>(stack_base) +
                      reserve_size - commit_start;
  st = ::NtAllocateVirtualMemoryEx(NtCurrentProcess(), &commit_addr,
                                   &commit_len, MEM_COMMIT, PAGE_READWRITE,
                                   nullptr, 0);
  if (!NT_SUCCESS(st)) {
    SIZE_T free_sz = 0;
    ::NtFreeVirtualMemory(NtCurrentProcess(), &stack_base, &free_sz,
                          MEM_RELEASE);
    return nullptr;
  }

  // Set PAGE_GUARD on the page just above the hard guard (stack growth trigger).
  if (commit_start > static_cast<uint8_t *>(stack_base) + 4096) {
    void *guard_addr = commit_start;
    SIZE_T guard_size = 4096;
    ULONG old = 0;
    ::NtProtectVirtualMemory(NtCurrentProcess(), &guard_addr, &guard_size,
                             PAGE_READWRITE | PAGE_GUARD, &old);
  }

  // Return the top of the stack (highest committed address), aligned.
  auto *stack_top = static_cast<uint8_t *>(stack_base) + reserve_size;
  return stack_top;
}

// Switch to a new stack and jump to the entry point. Does not return.
[[noreturn]] void exec_switch_stack_and_jump(void *stack_top,
                                             void *entry_point) {
  // RSP must be 16-byte aligned, then subtract 8 for the "call" return
  // address slot (x64 ABI: RSP is 16n+8 after a call instruction).
  auto rsp = reinterpret_cast<uintptr_t>(stack_top);
  rsp &= ~uintptr_t{15}; // Align to 16.
  rsp -= 8;               // Simulate return address push.
  *reinterpret_cast<uintptr_t *>(rsp) = 0; // NULL return address.

  __asm__ volatile(
      "movq %0, %%rsp\n\t"
      "xorq %%rbp, %%rbp\n\t" // Clear frame pointer.
      "xorq %%rcx, %%rcx\n\t" // Zero parameter registers.
      "xorq %%rdx, %%rdx\n\t"
      "xorq %%r8, %%r8\n\t"
      "xorq %%r9, %%r9\n\t"
      "jmpq *%1\n\t"
      :
      : "r"(rsp), "r"(entry_point)
      : "memory");
  __builtin_unreachable();
}

// Perform the actual image swap and jump. Does not return.
// All threads are already quiesced. All teardown is complete.
[[noreturn]] void exec_swap_image_and_jump(ExecImage &img) {
  PEB *peb = NtCurrentPeb();
  auto *old_base = static_cast<uint8_t *>(peb->ImageBaseAddress);
  uintptr_t base_addr = reinterpret_cast<uintptr_t>(old_base);

  // Step 1: Clear extra VA if target is larger.
  exec_clear_extra_va(base_addr, img.old_image_size, img.image_size);

  // Step 2: Unmap old EXE image. This is safe because we're executing
  // from c.dll, not the EXE.
  NTSTATUS st = ::NtUnmapViewOfSectionEx(NtCurrentProcess(), old_base, 0);
  if (!NT_SUCCESS(st))
    ::NtTerminateProcess(NtCurrentProcess(), 127);

  // Step 3: Remap target at the SAME base address.
  // MEM_DIFFERENT_IMAGE_BASE_OK allows SEC_IMAGE at non-preferred address.
  void *map_addr = old_base;
  SIZE_T view_size = 0;
  st = ::NtMapViewOfSectionEx(img.section, NtCurrentProcess(), &map_addr,
                              nullptr, &view_size, MEM_DIFFERENT_IMAGE_BASE_OK,
                              PAGE_READONLY, nullptr, 0);
  if (!NT_SUCCESS(st))
    ::NtTerminateProcess(NtCurrentProcess(), 127);

  auto *new_base = static_cast<uint8_t *>(map_addr);

  // Step 4: Apply IAT patches (imports resolved pre-swap).
  exec_apply_iat_patches(new_base, img.patches, img.patch_count);

  // Step 5: Register .pdata for exception handling.
  if (img.pdata_rva && img.pdata_count) {
    ::RtlAddFunctionTable(
        reinterpret_cast<PRUNTIME_FUNCTION>(new_base + img.pdata_rva),
        img.pdata_count, reinterpret_cast<DWORD64>(new_base));
  }

  // Step 6: Initialize TLS for the new image (Win32 target support).
  exec_init_tls(new_base);

  // Step 7: Register CFG call targets (Win32 target support).
  exec_register_cfg_targets(new_base);

  // Step 8: Close the section handle (no longer needed).
  ::NtClose(img.section);
  img.section = nullptr;

  // Step 9: Close the file handle.
  if (img.file) {
    ::NtClose(img.file);
    img.file = nullptr;
  }

  // Flush instruction cache and serialize memory after all image mutations
  // (IAT patches, TLS init, CFG registration). On x86-64 the I-cache is
  // coherent, but NtFlushInstructionCache also acts as a full memory barrier
  // and TLB flush — required after NtProtectVirtualMemory + writes to
  // SEC_IMAGE pages. Without this, the CPU may execute stale page-table
  // entries or see pre-COW data, causing ACCESS_VIOLATION in the new image.
  ::NtFlushInstructionCache(NtCurrentProcess(), new_base, img.image_size);

  // Step 10: Allocate new stack and jump to entry point.
  void *stack_top = exec_allocate_stack(img.img_info.MaximumStackSize,
                                        img.img_info.CommittedStackSize);
  if (!stack_top)
    ::NtTerminateProcess(NtCurrentProcess(), 127);

  void *entry = reinterpret_cast<void *>(
      reinterpret_cast<uintptr_t>(new_base) + img.entry_rva);

  // Global memory barrier: flush all CPUs' store buffers and TLB entries.
  // After unmap → remap → IAT patch → VirtualProtect cycles, stale TLB
  // entries or store-buffer residue from the old mapping can cause AV when
  // jumping to the new image. NtFlushProcessWriteBuffers sends an IPI to
  // all processors, guaranteeing all prior writes (IAT patches, protection
  // changes) are globally visible before we execute the remapped pages.
  ::NtFlushProcessWriteBuffers();

  exec_switch_stack_and_jump(stack_top, entry);
}

// =====================================================================
// §7  Self-Hollow Orchestrator
// =====================================================================

// Attempt PID-preserving self-hollow exec.
// Does not return on success (jumps to new entry point).
// Returns negative errno on failure (caller should fall back to zombie relay).
int exec_self_hollow(const WCHAR *nt_path, size_t nt_path_len,
                     const WCHAR *win32_path, size_t win32_path_chars,
                     char *const argv[], char *const envp[]) {
  ExecImage img;
  auto guard = cpp::make_scope_guard([&] { img.cleanup(); });

  // Phase 1: Open and validate target.
  int err = exec_open_and_validate(nt_path, nt_path_len, img);
  if (err)
    return err;

  // Phase 2: Map temp, resolve imports, build IAT patch table.
  err = exec_resolve_imports(img);
  if (err)
    return err;

  // Phase 3: Update LDR entry and PEB (ProcessParameters).
  err = exec_update_process_state(img, win32_path, win32_path_chars, argv,
                                  envp);
  if (err)
    return err;

  // Phase 4: Unmap temp mapping (no longer needed — patches are in the table).
  if (img.temp_base) {
    ::NtUnmapViewOfSectionEx(NtCurrentProcess(), img.temp_base, 0);
    img.temp_base = nullptr;
  }

  // ===== POINT OF NO RETURN =====
  // Everything below is irreversible. If anything fails after this point,
  // the process is terminated — there is no fallback.
  guard.dismiss(); // Don't cleanup on scope exit; we manage manually.

  // Phase 5: Pre-swap teardown (POSIX exec semantics).
  //
  // Ordering rationale:
  //   5a. Flush streams + close CLOEXEC fds (may trigger I/O)
  //   5b. POSIX signal reset (caught → SIG_DFL, pending cleared)
  //   5c. POSIX timer teardown (cancel+close before quiesce — callbacks
  //       may be in-flight on reactor drain threads)
  //   5d. Quiesce threads (freeze all non-current — after timers so
  //       reactor drain threads aren't frozen mid-callback)
  //   5e. Dead thread cleanup: reset locks held by frozen threads,
  //       flush allocator caches back to central pool
  //   5f. Old image TLS/FLS/LDR cleanup
  //
  // VEH is deliberately NOT removed: c.dll survives exec, so VEH
  // filters (signal delivery, mmap demand-commit, mlock onfault) remain
  // valid. The design doc §15.8 incorrectly assumed c.dll's .bss is
  // destroyed by the EXE unmap — it isn't.

  // 5a. Flush buffered FILE* writes and close O_CLOEXEC fds (POSIX).
  exec_flush_streams();
  exec_close_cloexec_fds();

  // 5b. POSIX signal reset: caught handlers → SIG_DFL; SIG_IGN preserved.
  // Clear process-pending and thread-pending. Reset sigaltstack.
  internal::signal_exec_reset_handlers();

  // 5c. POSIX timer teardown: invalidate all timer_create timers and
  // disarm setitimer. Must happen before quiescing — timer callbacks
  // run on reactor drain threads which must be free to complete.
  internal::setitimer_exec_teardown();
  internal::timer_create_exec_teardown();

  // 5d. Freeze all non-current threads.
  exec_cleanup_fls();         // Deallocate stale FLS without callbacks.
  exec_quiesce_threads();

  // 5e. Dead thread resource cleanup. All non-current threads are now
  // frozen. Reset locks they might hold and flush their allocator caches
  // back to the central pool so the new image can allocate.
  //
  // These are safe to call single-threaded — they just reset atomic lock
  // words and walk lists. No kernel objects are created.
  internal::env_fork_reinit();             // env lock (build_environ needs this)
  internal::alloc_exec_reinit();           // allocator: full reset for exec
  internal::mapping_table_fork_reinit();   // mapping table locks
  internal::mmap_lock_fork_reinit();       // mmap RW lock
  internal::mlock_policy_fork_reinit();    // mlock state
  internal::pkey_fork_reinit();            // pkey table lock
  internal::ofd_pool_fork_reinit();        // OFD slab pool lock
  internal::file_pool_fork_reinit();       // FILE slab pool lock
  internal::wait_slot_fork_reinit();       // wait slot pool lock
  internal::lifecycle_fork_reinit();       // lifecycle pool + cancel state
  internal::thread_self_fork_reinit();     // main thread attrib
  internal::robust_pool_fork_reinit();     // robust mutex pool lock
  internal::thread_ring_fork_reinit();     // per-thread IoRing pool lock
  internal::thread_storage_fork_reinit();  // thread start storage pool lock
  internal::named_semaphore_fork_reinit(); // named semaphore pool lock
  internal::fd_table_fork_reinit();        // fd table pool lock
  internal::brk_fork_reinit();             // brk state
  internal::dlfcn_fork_reinit();           // GlobalModuleSet seqlock

  // 5f. Old image cleanup.
  {
    PEB *peb = NtCurrentPeb();
    exec_teardown_old_tls(
        static_cast<uint8_t *>(peb->ImageBaseAddress));
  }

  // Update LDR module names to reflect the new executable.
  // Build full NT device path for FullDllName (e.g. \Device\Harddisk...).
  // We use the Win32 path (already available) since that's what
  // GetModuleFileNameW and dladdr read.
  {
    LDR_DATA_TABLE_ENTRY *ldr = find_exe_ldr_entry();
    exec_update_ldr_names(ldr, win32_path, win32_path_chars);
  }

  // Phase 6: Image swap + jump (does not return).
  exec_swap_image_and_jump(img);
}

// =====================================================================
// §8  Setuid Check
// =====================================================================

// S_ISUID/S_ISGID bit check on the target binary via $LXMOD/$LXUID/$LXGID
// extended attributes. Only effective if we have SeTcbPrivilege.
void check_setuid_bits(const WCHAR *wpath, int wpath_len) {
  if (g_pcb.identity.privilege_level.load(cpp::MemoryOrder::RELAXED) <
      windows_identity::PRIV_TCB)
    return;

  WCHAR nt_buf[266]; // \??\ + 260 + NUL
  nt_buf[0] = L'\\';
  nt_buf[1] = L'?';
  nt_buf[2] = L'?';
  nt_buf[3] = L'\\';
  for (int i = 0; i < wpath_len; ++i)
    nt_buf[4 + i] = wpath[i];
  int nt_len = 4 + wpath_len;
  nt_buf[nt_len] = L'\0';

  UNICODE_STRING us;
  us.Length = static_cast<USHORT>(nt_len * sizeof(WCHAR));
  us.MaximumLength = static_cast<USHORT>((nt_len + 1) * sizeof(WCHAR));
  us.Buffer = nt_buf;

  OBJECT_ATTRIBUTES oa = {};
  oa.Length = sizeof(oa);
  oa.ObjectName = &us;
  oa.Attributes = OBJ_CASE_INSENSITIVE;

  IO_STATUS_BLOCK iosb = {};
  HANDLE h = nullptr;
  NTSTATUS status = ::NtCreateFile(
      &h, FILE_READ_EA, &oa, &iosb, nullptr, 0,
      FILE_SHARE_READ | FILE_SHARE_DELETE, FILE_OPEN,
      FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);
  if (!NT_SUCCESS(status))
    return;

  alignas(4) UCHAR qbuf[sizeof(FILE_GET_EA_INFORMATION) + 6];
  auto *q = reinterpret_cast<FILE_GET_EA_INFORMATION *>(qbuf);
  q->NextEntryOffset = 0;
  q->EaNameLength = 6;
  __builtin_memcpy(q->EaName, "$LXMOD", 7);

  alignas(4) UCHAR rbuf[sizeof(FILE_FULL_EA_INFORMATION) + 6 + 4];
  iosb = {};
  status = ::NtQueryEaFile(h, &iosb, rbuf, sizeof(rbuf), TRUE, qbuf,
                           sizeof(qbuf), nullptr, TRUE);
  if (!NT_SUCCESS(status)) {
    ::NtClose(h);
    return;
  }

  auto *ea = reinterpret_cast<FILE_FULL_EA_INFORMATION *>(rbuf);
  if (ea->EaValueLength < 4) {
    ::NtClose(h);
    return;
  }

  UCHAR *vp = rbuf + __builtin_offsetof(FILE_FULL_EA_INFORMATION, EaName) +
              ea->EaNameLength + 1;
  ULONG raw_mode;
  __builtin_memcpy(&raw_mode, vp, 4);

  bool has_suid = (raw_mode & 04000) != 0;
  bool has_sgid = (raw_mode & 02000) != 0;

  if (!has_suid && !has_sgid) {
    ::NtClose(h);
    return;
  }

  uid_t file_uid = 0;
  gid_t file_gid = 0;

  auto read_ea_u32 = [&](const char *name, ULONG *out) -> bool {
    q->EaNameLength = 6;
    __builtin_memcpy(q->EaName, name, 7);
    iosb = {};
    status = ::NtQueryEaFile(h, &iosb, rbuf, sizeof(rbuf), TRUE, qbuf,
                             sizeof(qbuf), nullptr, TRUE);
    if (!NT_SUCCESS(status))
      return false;
    ea = reinterpret_cast<FILE_FULL_EA_INFORMATION *>(rbuf);
    if (ea->EaValueLength < 4)
      return false;
    vp = rbuf + __builtin_offsetof(FILE_FULL_EA_INFORMATION, EaName) +
         ea->EaNameLength + 1;
    __builtin_memcpy(out, vp, 4);
    return true;
  };

  ULONG val;
  if (has_suid && read_ea_u32("$LXUID", &val))
    file_uid = static_cast<uid_t>(val);
  if (has_sgid && read_ea_u32("$LXGID", &val))
    file_gid = static_cast<gid_t>(val);

  ::NtClose(h);
  windows_identity::set_exec_ids(file_uid, file_gid, has_suid, has_sgid);
}

// =====================================================================
// §9  execvp Helpers (PATH Search)
// =====================================================================

constexpr int MAX_CANDIDATE_PATH = 1024;

bool has_dir_separator(const char *file) {
  for (const char *p = file; *p; ++p) {
    if (*p == '/' || *p == '\\')
      return true;
  }
  return false;
}

bool has_extension(const char *file) {
  const char *dot = nullptr;
  for (const char *p = file; *p; ++p) {
    if (*p == '.')
      dot = p;
    else if (*p == '/' || *p == '\\')
      dot = nullptr;
  }
  return dot != nullptr;
}

size_t str_len(const char *s) {
  size_t len = 0;
  while (s[len])
    ++len;
  return len;
}

const char *find_path_env() {
  return internal::env_get("PATH");
}

int build_candidate(char *buf, int buf_size, const char *dir, size_t dir_len,
                    const char *file, size_t file_len) {
  int pos = 0;
  for (size_t i = 0; i < dir_len; ++i) {
    if (pos >= buf_size - 1)
      return 0;
    buf[pos++] = dir[i];
  }
  if (pos > 0 && buf[pos - 1] != '\\' && buf[pos - 1] != '/') {
    if (pos >= buf_size - 1)
      return 0;
    buf[pos++] = '\\';
  }
  for (size_t i = 0; i < file_len; ++i) {
    if (pos >= buf_size - 1)
      return 0;
    buf[pos++] = file[i];
  }
  buf[pos] = '\0';
  return pos;
}

// =====================================================================
// §9a  Shebang (#!) Script Interpreter Detection
// =====================================================================

// Shebang constants and parser are in shebang.h (shared with posix_spawn).

} // anonymous namespace

// =====================================================================
// §10  Public API
// =====================================================================

using process_utils::utf8_to_wide;
using process_utils::utf8_to_wide_len;

namespace internal {

// Internal execve with shebang recursion depth tracking.
static intptr_t execve_impl(const char *path, char *const argv[],
                            char *const envp[], int depth) {
  if (!path || !path[0]) {
    return -EINVAL;
  }

  if (depth > SHEBANG_MAX_DEPTH)
    return -ELOOP;

  // Convert path to NT format.
  auto nt_path_s = path_scratch();
  if (!nt_path_s) {
    return -ENOMEM;
  }
  WCHAR *nt_path = nt_path_s.data();
  size_t nt_path_len = to_nt_path(path, nt_path, nt_path_s.size());
  if (nt_path_len == 0) {
    return -EINVAL;
  }

  // POSIX EISDIR: reject directory targets.
  {
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    init_object_attributes(&oa, &us, nt_path, nt_path_len);

    FILE_BASIC_INFORMATION basic = {};
    NTSTATUS status = ::NtQueryAttributesFile(&oa, &basic);
    if (NT_SUCCESS(status) && (basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY))
      return -EISDIR;
  }

  // E2BIG check: command line and environment size limits.
  SIZE_T cmdline_bytes = process_utils::build_cmdline(argv, nullptr);
  SIZE_T env_bytes = process_utils::build_env_block(envp, nullptr);
  constexpr SIZE_T MAX_CMDLINE_BYTES = 32767 * sizeof(WCHAR);
  constexpr SIZE_T MAX_ENV_BYTES = 32767 * sizeof(WCHAR);
  if (cmdline_bytes > MAX_CMDLINE_BYTES ||
      (env_bytes > 0 && env_bytes > MAX_ENV_BYTES))
    return -E2BIG;

  // Compute Win32-style path (strip \??\ prefix from NT path).
  const WCHAR *win32_path = nt_path_len > 4 ? nt_path + 4 : nt_path;
  size_t win32_path_chars =
      nt_path_len > 4 ? nt_path_len - 4 : nt_path_len;

  // Setuid check.
  check_setuid_bits(win32_path, static_cast<int>(win32_path_chars));

  // Self-hollow: replace process image in-place, preserving PID.
  // Does not return on success (jumps to new entry point).
  // Returns negative errno on failure.
  intptr_t ret = exec_self_hollow(nt_path, nt_path_len, win32_path,
                                  win32_path_chars, argv,
                                  envp ? envp : LIBC_NAMESPACE::environ);
  // If exec_self_hollow returned ENOEXEC, try shebang interpretation.
  // POSIX: "If the process image file has appropriate permission and is in a
  // format that is executable but not supported by this implementation (such
  // as a shell script), the behavior is unspecified" — but implementations
  // conventionally check for #! and invoke the named interpreter.
  if (ret == -ENOEXEC) {
    char interp[SHEBANG_MAX_PATH];
    char interp_arg[SHEBANG_MAX_PATH];
    int interp_len = 0, arg_len = 0;

    if (internal::parse_shebang(nt_path, nt_path_len, interp, &interp_len,
                                interp_arg, &arg_len) == 0) {
      // Build new argv: [interpreter, optional_arg, script_path, argv[1:]]
      // Count original argv entries (skip argv[0] — replaced by script path).
      int orig_argc = 0;
      if (argv) {
        for (int i = 0; argv[i]; ++i)
          ++orig_argc;
      }

      // New argv length: interpreter + optional_arg + script_path + tail + NULL
      int new_argc = 1 + (arg_len > 0 ? 1 : 0) + 1 +
                     (orig_argc > 1 ? orig_argc - 1 : 0);
      // Stack-allocate for reasonable sizes, otherwise bail.
      constexpr int MAX_SHEBANG_ARGV = 256;
      if (new_argc >= MAX_SHEBANG_ARGV)
        return -E2BIG;

      char *new_argv[MAX_SHEBANG_ARGV];
      int pos = 0;
      new_argv[pos++] = interp;
      if (arg_len > 0)
        new_argv[pos++] = interp_arg;
      new_argv[pos++] = const_cast<char *>(path); // Script path.
      // Append original argv[1:].
      if (argv) {
        for (int i = 1; i < orig_argc; ++i)
          new_argv[pos++] = argv[i];
      }
      new_argv[pos] = nullptr;

      return execve_impl(interp, new_argv, envp, depth + 1);
    }
  }

  return ret;
}

intptr_t execve(const char *path, char *const argv[], char *const envp[]) {
  return execve_impl(path, argv, envp, 0);
}

intptr_t execvp(const char *file, char *const argv[]) {
  if (!file || !file[0])
    return -ENOENT;

  if (has_dir_separator(file))
    return internal::execve(file, argv, LIBC_NAMESPACE::environ);

  const char *path_env = find_path_env();
  if (!path_env || !path_env[0])
    return internal::execve(file, argv, LIBC_NAMESPACE::environ);

  size_t file_len = str_len(file);
  bool try_exe = !has_extension(file);
  int last_errno = ENOENT;

  const char *p = path_env;
  while (true) {
    const char *end = p;
    while (*end && *end != ';')
      ++end;

    size_t dir_len = static_cast<size_t>(end - p);

    char candidate[MAX_CANDIDATE_PATH];
    int cand_len =
        build_candidate(candidate, MAX_CANDIDATE_PATH, p, dir_len, file,
                        file_len);

    if (cand_len > 0) {
      intptr_t ret = internal::execve(candidate, argv, LIBC_NAMESPACE::environ);
      last_errno = static_cast<int>(-ret);

      if (try_exe && (last_errno == ENOENT || last_errno == ENOTDIR) &&
          cand_len + 4 < MAX_CANDIDATE_PATH) {
        candidate[cand_len] = '.';
        candidate[cand_len + 1] = 'e';
        candidate[cand_len + 2] = 'x';
        candidate[cand_len + 3] = 'e';
        candidate[cand_len + 4] = '\0';

        ret = internal::execve(candidate, argv, LIBC_NAMESPACE::environ);
        last_errno = static_cast<int>(-ret);
      }

      if (last_errno != ENOENT && last_errno != ENOTDIR)
        break;
    }

    if (!*end)
      break;
    p = end + 1;
  }

  return -last_errno;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
