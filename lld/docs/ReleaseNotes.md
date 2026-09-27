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
