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

* `/delay:unload` is honored rather than ignored: the image gets a copy of the
  delay-load import address table in `UnloadDelayImportTable`, which
  `__FUnloadDelayLoadedDLL2` restores before it frees the library.
  `/delay:nobind` is accepted; no image LLD writes has a bound delay-load
  import table. Any other argument is an error.

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
* `-start-stop-symbols` defines a referenced `__start_X` and `__stop_X`, where
  `X` is a C identifier, around the input sections named `X` or `X$*`, as ELF
  linkers do, and `-boundary-symbols` defines a referenced `_etext`, `_edata`
  and `_end` and their unprefixed forms. Both are off by default.

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
