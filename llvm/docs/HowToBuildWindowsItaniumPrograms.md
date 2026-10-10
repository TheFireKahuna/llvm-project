# How to Build Windows Itanium Programs

## Introduction

Windows Itanium (`x86_64-unknown-windows-itanium` and
`aarch64-unknown-windows-itanium`) is a Windows environment that uses the
Itanium C++ ABI instead of the Microsoft C++ ABI. Its programs are COFF/PE
images that run on the Universal CRT (`ucrtbase.dll`) and the Win32 API, and
use the Windows SDK headers and import libraries. No Visual C++ header or
library is used.

The stack is:

- Clang, with the Itanium C++ ABI and CodeView debug information.
- LLD (`lld-link`), the only supported linker.
- libc++ and libc++abi.
- libunwind, which unwinds through the Windows SEH unwind tables. C++
  exceptions use the Itanium personality over SEH unwinding; SJLJ and DWARF
  exceptions are not available.
- compiler-rt: the builtins, and `wincrt`, the start-up library. Microsoft's
  C runtime is split into the Universal CRT (the C library) and vcruntime
  (start-up, compiler support and the Microsoft C++ ABI). Windows Itanium keeps
  the Universal CRT and replaces vcruntime: wincrt provides the entry points,
  static initialization and termination (`__cxa_atexit` and `atexit`),
  thread-local storage, the stack protector, Control Flow Guard support,
  delay-load imports and the load configuration directory, and libc++abi and
  libunwind provide the C++ runtime.

The supported architectures are x86-64 and AArch64.

## Building the toolchain

The cache file `clang/cmake/caches/WindowsItanium-runtimes.cmake` configures
Clang, LLD and, for each triple in `LLVM_RUNTIME_TARGETS`, the builtins,
wincrt, libunwind, libc++abi and libc++ as per-target runtimes
(`LLVM_ENABLE_PER_TARGET_RUNTIME_DIR`). The runtimes are built with the Clang
being built. From the `llvm-project` directory:

```bash
cmake -G Ninja -B build -C clang/cmake/caches/WindowsItanium-runtimes.cmake llvm
ninja -C build
```

`LLVM_RUNTIME_TARGETS` defaults to `x86_64-unknown-windows-itanium`. To build
the runtimes for AArch64 as well, list both triples:

```bash
-DLLVM_RUNTIME_TARGETS="x86_64-unknown-windows-itanium;aarch64-unknown-windows-itanium"
```

The first triple in the list becomes the default target of the Clang that is
built.

The cache builds libc++abi into both the libc++ DLL and the static libc++
library, links every runtime with compiler-rt's builtins, enables libc++'s
`fast` hardening mode, and builds no sanitizer runtimes.

On a host other than Windows, the runtimes are cross-compiled, and the Clang
being built must be given the Windows SDK, for example with
`-Xmicrosoft-windows-sys-root` in a configuration file beside it.

### A self-hosted toolchain

`clang/cmake/caches/WindowsItanium-toolchain.cmake`, loaded after the runtimes
cache, configures a two-stage build. The first stage, built on Windows with
any compiler, builds Clang, LLD and the runtimes; the second stage is built by
the first, targets Windows Itanium itself, and links those runtimes. It needs
zlib, zstd and libxml2 for both stages:

```bash
cmake -G Ninja -B build \
  -C clang/cmake/caches/WindowsItanium-runtimes.cmake \
  -C clang/cmake/caches/WindowsItanium-toolchain.cmake \
  -DCMAKE_PREFIX_PATH=<zlib, zstd and libxml2 for the Windows host> \
  -DBOOTSTRAP_CMAKE_PREFIX_PATH=<zlib, zstd and libxml2 for the toolchain> \
  llvm
ninja -C build stage2-distribution
```

## Compiling and linking

The driver finds the Universal CRT and the Windows SDK as it does for MSVC
targets, or through `-Xmicrosoft-windows-sdk-root`,
`-Xmicrosoft-windows-sdk-version` and `-Xmicrosoft-windows-sys-root`. A
Visual Studio installation is not needed, and the `LIB` environment variable
is ignored.

```bash
clang++ --target=x86_64-unknown-windows-itanium hello.cpp -o hello.exe
clang++ --target=x86_64-unknown-windows-itanium -shared foo.cpp -o foo.dll
```

`-shared` writes the DLL's import library as `foo.dll.lib`, so that it can sit
beside a static library `foo.lib`. `-lname` looks for `libname.dll.lib`, then
`libname.lib`, in the library directories, and otherwise passes `name.lib` to
the linker.

`clang-cl` works with the same `--target=`, and its options are translated as
for MSVC targets (for example, `/LD` builds a DLL).

### Libraries

The driver passes the runtime libraries to `lld-link` as `-defaultlib:`, so
that definitions in the program's own objects and libraries take precedence:

- `libc++.dll.lib`, for C++ programs, and `libunwind.dll.lib`;
- compiler-rt's builtins;
- `clang_rt.wincrt.lib`, `clang_rt.wincrt_dynamic.lib`,
  `clang_rt.ucrt_memory.lib` and `clang_rt.aligned_alloc.lib`;
- `ucrt.lib`, `kernel32.lib`, `ntdll.lib`, `oldnames.lib` and
  `onecore_apiset.lib`, and with `-mwindows`, which selects the GUI
  subsystem, also `user32.lib`.

`onecore_apiset.lib`, the SDK's API-set umbrella library, comes after
`kernel32.lib`. It binds registry, security, service, COM, version-information,
socket and shell-core functions such as `CommandLineToArgvW` to the API sets
that `kernelbase.dll`, `sechost.dll`, `combase.dll` and `shcore.dll` host,
rather than to `advapi32.dll`, `ole32.dll` and `shell32.dll`, which load
`msvcrt.dll` or connect the process to win32k. A console program is therefore
win32k-free unless it names a library that connects it:

- windowing, messages, `MessageBox`, `wsprintf`, the clipboard and
  `GetSystemMetrics` need `-luser32`, which `-mwindows` adds, and GDI needs
  `-lgdi32` in every program;
- `ShellExecute`, `SHFileOperation`, `SHGetFileInfo`, `SHGetKnownFolderPath`
  and the rest of the shell need `-lshell32`; the environment
  (`USERPROFILE`, `LOCALAPPDATA`) and `GetUserProfileDirectoryW` find the
  common folders without it;
- `CoInitialize`, `OleInitialize`, monikers, drag and drop need `-lole32`;
  `CoInitializeEx` and `CoCreateInstance` need nothing;
- the legacy `advapi32.dll` functions the umbrella omits, such as event-log
  reading, EFS and LSA account management, need `-ladvapi32`.

A library named on the command line, such as `-ladvapi32`, is searched before
the defaults, so the functions it offers bind to its DLL. A
`#pragma comment(lib, ...)` directive is searched after them and does not
change those bindings. Use `onecore_apiset.lib`, not `onecore.lib`, which names
the classic DLLs again.

`clang_rt.wincrt_dynamic.dll` holds the parts of wincrt that serve the whole
process, such as the termination registries and `exit`, and every image
imports them from it. The runtime DLLs a program imports from, such as
`libc++.dll`, `libunwind.dll` and `clang_rt.wincrt_dynamic.dll`, must be beside
it or on `PATH`.

With `-static`, the program links `libc++.lib`, `libunwind.lib` and
`clang_rt.wincrt_static.lib` instead. The Universal CRT is always linked
dynamically.

`clang_rt.ucrt_memory.lib` imports the functions that `ucrtbase.dll` exports
but `ucrt.lib` does not, such as `memcpy`, `setjmp` and `longjmp`, which
Visual C++ imports through `vcruntime.lib`. `oldnames.lib` maps the POSIX
names of Universal CRT functions (`open`, `strdup` and so on) to their
underscored names. An empty `m.lib` lets build systems pass `-lm`.

### Defaults

- **Exports.** A definition with explicit default visibility
  (`__attribute__((visibility("default")))`) is exported from its image, and a
  declaration with it is imported. `__declspec(dllexport)` and
  `__declspec(dllimport)` are honoured as well. Variables are not
  auto-imported unless `-fauto-import` is given.
- **Control Flow Guard** is on (`-mguard=cf`). `-mguard=cf-nochecks` emits only
  the table of address-taken functions, and `-mguard=none` turns it off. An
  executable suppresses its exports as call targets.
- **CET shadow stack.** Every x86-64 image is marked CET-compatible, and every
  x86-64 object lists its EH continuation targets.
- **KCFI** checks indirect calls (`-fno-sanitize=kcfi` turns the checks off).
  With LTO, CFI also checks the virtual, member and indirect calls of the code
  LTO sees whole.
  `-fms-hotpatch` is not supported.
- **Stack protector.** `-fstack-protector-strong` is the default.
- **Automatic variables** are zero-initialized (`-ftrivial-auto-var-init=zero`).
- **Unwind tables** are always emitted, and every function and data item has
  its own section.
- **CPU.** x86-64 code targets `x86-64-v3` with AES-NI, PCLMULQDQ, FSGSBASE,
  ADX, RDRAND, RDSEED, CLFLUSHOPT and XSAVEC unless `-march=` is given.
- **Microsoft extensions** (`-fms-extensions`) are on, since the Windows SDK
  headers need them. `_MSC_VER` is not defined; `_WIN32_ITANIUM` is.
- **Manifest.** Every executable embeds a manifest that selects the segment
  heap, and its start-up fails if the process heap is not a segment heap.

## Unsupported configurations

- The static Universal CRT, `libucrt.lib`, is not supported. wincrt starts up
  the Universal CRT of `ucrtbase.dll`, which `ucrt.lib` imports; naming
  `libucrt.lib` would give the image a second Universal CRT, whose state
  nothing initializes.
- The Visual C++ runtime libraries (`msvcrt`, `vcruntime`, `libcmt` and their
  debug forms) and the debug Universal CRT (`ucrtd`) are never linked: the
  driver removes them with `-nodefaultlib:` when objects compiled by clang-cl
  or MSVC name them.
- Linkers other than `lld-link`.
- 32-bit x86 and 32-bit Arm.

## Testing

Clang's and LLD's tests run as for any target:

```bash
ninja -C build check-clang check-lld
```

On a Windows host the runtimes are not cross-compiled, and their tests run:

```bash
ninja -C build check-cxx check-cxxabi check-unwind check-builtins
```

wincrt's tests are C programs in `compiler-rt/test/builtins/Unit/windows`,
which run under `check-builtins` (the cache enables `COMPILER_RT_BUILD_CRT`).
With several runtime targets, `check-cxx-<triple>` and the like test one of
them.
