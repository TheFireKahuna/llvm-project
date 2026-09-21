.. If you want to modify sections/contents permanently, you should modify both
   ReleaseNotes.rst and ReleaseNotesTemplate.txt.

===========================
lld |release| Release Notes
===========================

.. contents::
    :local:

.. only:: PreRelease

  .. warning::
     These are in-progress notes for the upcoming LLVM |release| release.
     Release notes for previous releases can be found on
     `the Download Page <https://releases.llvm.org/download.html>`_.

Introduction
============

This document contains the release notes for the lld linker, release |release|.
Here we describe the status of lld, including major improvements
from the previous release. All lld releases may be downloaded
from the `LLVM releases web site <https://llvm.org/releases/>`_.

Non-comprehensive list of changes in this release
=================================================

ELF Improvements
----------------

* Added ``--bp-compression-sort-section=<glob>[=<layout_priority>[=<match_priority>]]``,
  replacing the old coarse ``--bp-compression-sort`` modes with a way to split
  input sections into multiple compression groups, run balanced partitioning
  independently per group, and leave out sections that are poor candidates for
  BP.
  ``layout_priority`` controls group placement order (lower value = placed
  first, default 0). ``match_priority`` resolves conflicts when multiple globs
  match the same section (lower value = higher priority; explicit priority
  beats positional last-match-wins; default: positional). In ELF, the glob
  matches input section names (e.g. ``.text.unlikely.code1``).

Breaking changes
----------------

COFF Improvements
-----------------

* ``/guard:cf`` images carry Control Flow Guard export suppression metadata:
  the guard tables use 5-byte entries, an exported function that is a valid
  target only because it is exported is marked export-suppressed, and an
  object without guard metadata that reads an import pointer lists that entry
  in the address-taken IAT table. ``/guard:exportsuppress`` enables the mode
  for the process; ``/guard:noexportsuppress`` clears it.

* A ``__imp_`` reference to a symbol defined in the image is now bound
  directly: the ``mov``, ``call`` and ``jmp`` forms (``adrp``/``ldr`` on
  AArch64) are rewritten to reference the definition, and the pointer is
  emitted only for references in other forms. An undefined ``__imp_X`` loads
  the archive member that defines ``X``, and takes the ``/alternatename``
  given to ``X``. LTO treats every definition in the link as final and drops
  the ``dllimport`` of such references itself.

* ``-import-slots`` makes static data that holds the address of an imported
  symbol an in-place import slot: the loader writes the address there through
  an import descriptor whose address table is the data itself, so the word
  needs neither a thunk nor a runtime pseudo relocation, and pointer identity
  holds across images. Read-only slots are laid out with the import address
  table, which the IAT data directory covers. A vtable entry is a slot like
  any other, so it holds the function's own address; an import thunk that
  only slots name is left out. A word holding an offset as well, which the
  loader cannot write, is written on x86-64 by a function the linker adds as
  the first C initializer, and a read-only one moves to ``.data``, as the
  pointer MSVC's compiler initializes at startup is writable. The option
  implies ``-auto-import``; a definition of ``X`` in the link is then
  preferred to an import library's entry for it.

* An object that reaches a thread-local variable of another image by its
  section-relative offset gets a diagnostic that says so: the undefined-symbol
  error names the record ``X$tls`` a DLL exports in place of the variable,
  and under ``-import-slots`` a DLL that exports the variable itself gets an
  error explaining that the offset cannot reach it.

* ``-delayload-protect`` gives the delay-load import address table a section
  of its own, as link.exe does, and marks the image
  ``IMAGE_GUARD_PROTECT_DELAYLOAD_IAT`` and
  ``IMAGE_GUARD_DELAYLOAD_IAT_IN_ITS_OWN_SECTION``. The loader then keeps that
  table read-only and opens it only while it resolves an import, so a table
  that indirect calls reach without a Control Flow Guard check is not writable
  for the life of the process. The descriptors and the import name table move
  to ``.rdata`` and the module handles stay in ``.data``, since the loader
  writes neither while the table is protected. The delay-load helper has to
  reach the table through the loader, or open the page around its own store.

* ``/delay:unload`` is honored rather than ignored: the image gets a copy of
  the delay-load address table in ``UnloadDelayImportTable``, which is what
  ``__FUnloadDelayLoadedDLL2`` restores before it frees the library.
  ``/delay:nobind`` is accepted and is already the behaviour, since no image
  this linker writes carries a bindable table; any other argument is an error.

MinGW Improvements
------------------

MachO Improvements
------------------

* ``--bp-compression-sort-section`` now accepts optional layout and match
  priorities (same syntax as ELF). In Mach-O, the glob matches the
  concatenated segment+section name (e.g. ``__TEXT__text``).

WebAssembly Improvements
------------------------

Fixes
#####
