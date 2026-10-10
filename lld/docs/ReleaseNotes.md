<!-- If you want to modify sections/contents permanently, you should modify both
ReleaseNotes.md and ReleaseNotesTemplate.txt. -->

(lld-release-release-notes)=

# lld {{ release | default("") }} Release Notes

```{contents}
:local: true
```

::::{only} PreRelease

:::{warning}
These are in-progress notes for the upcoming LLVM {{ release | default("") }} release.
Release notes for previous releases can be found on
[the Download Page](https://releases.llvm.org/download.html).
:::
::::

## Introduction

This document contains the release notes for the lld linker, release {{ release | default("") }}.
Here we describe the status of lld, including major improvements
from the previous release. All lld releases may be downloaded
from the [LLVM releases web site](https://llvm.org/releases/).

## Non-comprehensive list of changes in this release

### ELF Improvements

### Breaking changes

### COFF Improvements

* In an import library LLD writes, a member that imports by name carries the
  export's index in the DLL's export name table as its hint, as link.exe
  writes it, so the loader finds the import without a binary search. Before,
  the hint was the export's ordinal, or 0 without an explicit one.

* `/delay:unload` is honored rather than ignored: the image gets a copy of the
  delay-load import address table in `UnloadDelayImportTable`, which
  `__FUnloadDelayLoadedDLL2` restores before it frees the library.
  `/delay:nobind` is accepted; no image LLD writes has a bound delay-load
  import table. Any other argument is an error.

* `/guard:cf` images carry Control Flow Guard export suppression metadata. An
  exported function that is a valid call target only because it is exported
  is marked export-suppressed, and the image declares the information
  complete with `IMAGE_GUARD_CF_EXPORT_SUPPRESSION_INFO_PRESENT`. The guard
  tables get a flag byte per entry when some entry has a flag.
  `/guard:exportsuppress` enables suppression for the process an executable
  starts, and `/guard:noexportsuppress` clears it.

* `/guardsym:<symbol>,S`, on the command line or in an object's `.drectve`
  section as the Universal CRT's objects and objects compiled from
  `__declspec(guard(suppress))` give it, is honored rather than ignored: the
  function stays in the Control Flow Guard function table, marked
  `IMAGE_GUARD_FLAG_FID_SUPPRESSED`, so that it is not a valid call target. A
  name in a directive may be a symbol local to its object. Under LTO, the
  directives that code generation gives are read from LTO's output. Identical
  code folding leaves a suppressed function out. Any other argument is ignored
  with a warning.

* With `/guard:ehcont`, an object compiled without EH continuation metadata is
  an error when its unwind data names a language handler other than
  `__GSHandlerCheck`, or when it references `_local_unwind`, as with link.exe's
  LNK2046 and LNK2047: its continuation targets would be missing from the
  table. SEH in a COMDAT is a warning instead, as with LNK4291, and the table
  lists the `__except` blocks of its scope tables. `/guard:nocf` turns off Control Flow Guard, the longjmp table and
  export suppression that earlier arguments turned on, so that
  `/guard:ehcont,nocf` asks for the EH continuation table alone, which a
  shadow stack checks continuations against whether or not calls are checked.
  The EH continuation fields of `_load_config_used` are checked whenever the
  table is asked for, not only with the longjmp table.

* With `/guard:cf`, an object without guard metadata has every import address
  table entry it references listed in the address-taken IAT table, since a
  call through an entry cannot be told from a read of it.

* With `/guard:cf`, the delay-load import address table starts a section of
  its own, `.didat`, as link.exe lays it out, and the image is marked
  `IMAGE_GUARD_PROTECT_DELAYLOAD_IAT` and
  `IMAGE_GUARD_DELAYLOAD_IAT_IN_ITS_OWN_SECTION`. The loader then keeps the
  table read-only and makes it writable only while it resolves an import, so
  a table that calls go through without a Control Flow Guard check is not
  writable for the life of the process. The descriptors and name table move to
  `.rdata`. Input sections named `.didat`, writable or not, follow the table
  in `.didat` in name order, starting on the next page. The loader makes the
  whole section read-only at load and reopens only the table's pages, so their
  data is read-only once the image is loaded. Merges into or out of `.didat`
  are errors in such an image. MinGW images, whose delay-load helper stores to
  the table directly, are unchanged. An image whose delay-load thunks call a
  helper whose object carries a link record saying that the helper writes the
  table only while it is writable (kind 16, `.linkprotecteddelayiat` in
  assembly) gets the same protection without `/guard:cf`, in a MinGW image
  too.

* The error for a DLL that would export more than 65535 symbols names the
  files that define the most exported symbols.

* An undefined `__imp_X` whose `X` is defined through a weak alias is no longer
  reported as unresolvable before LTO, as the link without LTO resolves it
  with a local import.

* `/lto-whole-program-visibility`, `/lto-validate-all-vtables-have-type-infos`
  and `/lto-known-safe-vtables` work as ELF's `--lto-whole-program-visibility`,
  `--lto-validate-all-vtables-have-type-infos` and `--lto-known-safe-vtables`
  do. A vtable the image exports stays public.

* A reference that binds to a definition in the image, or a dllimport
  reference to one, is final for LTO, which emits it as a direct reference,
  since a PE image has no symbol preemption.

* `-start-stop-symbols` defines a referenced `__start_X` and `__stop_X`, where
  `X` is a C identifier, around the input sections named `X` or `X$*`, as ELF
  linkers do, and `-boundary-symbols` defines a referenced `_etext`, `_edata`
  and `_end` and their unprefixed forms. Both are off by default.

* `-import-slots` selects the import binding model of Windows Itanium and
  NT-POSIX. Under it, an undefined `__imp_X` that no input provides under that
  name loads the archive member that defines `X`, as a reference to `X` would,
  and binds to it through a local pointer with warning LNK4217, where
  link.exe and LLD otherwise report an undefined symbol. The member is also
  loaded after LTO, for the calls through `__imp_` that code generation adds
  to library functions under `-fno-plt`. MinGW mode no longer turns on
  `-runtime-pseudo-reloc` under it, and asking for pseudo relocations with it
  is an error.

### MinGW Improvements

### MachO Improvements

* `__objc_stubs` entries are now ordered by the priority of the sections that
  call them, so that stubs reached from prioritized code are laid out together.
  This applies whenever section priorities exist, such as with `-order_file`.

* Added `--warn-missing-subsections-via-symbols` and
  `--no-warn-missing-subsections-via-symbols` to lld to warn when input object
  files lack the `MH_SUBSECTIONS_VIA_SYMBOLS` flag, which prevents
  dead-stripping and subsection splitting.

### WebAssembly Improvements

* Added support for resolving and merging common data symbols (allocating them
  into .bss.common in executable/shared module links, or merging them with max
  size/alignment in relocatable -r links). See
  https://github.com/WebAssembly/tool-conventions/pull/267

#### Fixes
