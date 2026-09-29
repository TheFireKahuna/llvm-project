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

* With `/guard:ehcont`, an object compiled without EH continuation metadata is
  an error when its unwind data names a language handler other than
  `__GSHandlerCheck`, or when it references `_local_unwind`, as with link.exe's
  LNK2046 and LNK2047: its continuation targets would be missing from the
  table.

* With `/guard:cf`, the delay-load import address table gets a section of its
  own, `.didat`, as link.exe lays it out, and the image is marked
  `IMAGE_GUARD_PROTECT_DELAYLOAD_IAT` and
  `IMAGE_GUARD_DELAYLOAD_IAT_IN_ITS_OWN_SECTION`. The loader then keeps the
  table read-only and makes it writable only while it resolves an import, so
  a table that calls go through without a Control Flow Guard check is not
  writable for the life of the process. The descriptors and name table move to
  `.rdata`. Input sections named `.didat` and merges into or out of `.didat`
  are errors in such an image. MinGW images, whose delay-load helper stores to
  the table directly, are unchanged.

* `-import-slots` selects the import binding model of Windows Itanium and
  NT-POSIX. Under it, an undefined `__imp_X` that no input provides under that
  name loads the archive member that defines `X`, as a reference to `X` would,
  and binds to it through a local pointer with warning LNK4217, where
  link.exe and LLD otherwise report an undefined symbol. The member is also
  loaded after LTO, for the calls through `__imp_` that code generation adds
  to library functions under `-fno-plt`.

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
