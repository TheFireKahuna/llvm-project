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
  given to ``X``. With ``-auto-import``, a definition of ``X`` in the link is
  preferred to an import library's entry for it. LTO treats every definition
  in the link as final and drops the ``dllimport`` of such references itself.

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
