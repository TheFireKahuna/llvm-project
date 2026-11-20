# REQUIRES: x86

## A GNU version script narrows the objects' exports (local: removes, global:
## keeps, by ld.lld's rules); /export and .def exports stay, and a global name
## nothing exports is reported.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-gnu syms.s -o syms.obj
# RUN: cat syms.s directives.s | \
# RUN:   llvm-mc -filetype=obj -triple=x86_64-windows-gnu -o directives.obj

## An exact name beats a wildcard and * comes last; extern "C++" patterns match
## demangled names, quoted ones exactly.
# RUN: lld-link -dll -noentry directives.obj -out:a.dll -version-script:a.map \
# RUN:   -export:bar -def:baz.def 2>&1 | FileCheck --check-prefix=WARN %s
# RUN: llvm-readobj --coff-exports a.dll | \
# RUN:   FileCheck --check-prefix=A --implicit-check-not=Name: %s
# RUN: lld-link -lldmingw -dll -noentry syms.obj -out:all.dll \
# RUN:   -export-all-symbols --version-script a.map 2>&1 | \
# RUN:   FileCheck --check-prefix=WARN %s
# RUN: llvm-readobj --coff-exports all.dll | \
# RUN:   FileCheck --check-prefix=ALL --implicit-check-not=Name: %s

# WARN: warning: version script assignment of 'global' to symbol 'missing' failed: symbol not exported
# WARN-NOT: warning:

# A:      Name: _Z1hv
# A-NEXT: RVA:
# A:      Name: _Z3quxv
# A:      Name: _ZN2ns1fEv
# A:      Name: bar
# A:      Name: baz
# A:      Name: foo

# ALL:      Name: _Z1hv
# ALL:      Name: _Z3quxv
# ALL:      Name: _ZN2ns1fEv
# ALL:      Name: foo

## Every script is read; an unnamed export stays; a twice-assigned name keeps
## ld.lld's first.
# RUN: lld-link -dll -noentry directives.obj -out:b.dll -version-script:b.map \
# RUN:   --version-script=c.map 2>&1 | FileCheck --check-prefix=REASSIGN %s
# RUN: llvm-readobj --coff-exports b.dll | \
# RUN:   FileCheck --check-prefix=B --implicit-check-not=Name: %s

# REASSIGN: warning: attempt to reassign symbol 'bar' of VER_NDX_LOCAL to VER_NDX_GLOBAL

# B:      Name: _Z1hv
# B:      Name: _Z3quxv
# B:      Name: _ZN2ns1fEv
# B:      Name: kept

## The reader's errors are ld.lld's.
# RUN: not lld-link -dll -noentry directives.obj -out:bad.dll \
# RUN:   -version-script:bad.map 2>&1 | FileCheck --check-prefix=BAD %s

# BAD: error: bad.map:1: EOF expected, but got trailing

#--- a.map
{
  global:
    foo;
    missing;
    extern "C++" { "qux()"; ns::*; "h()" };
  local:
    extern "C++" { h* };
    *;
};

#--- b.map
{ global: bar; local: ba*; bar; };

#--- c.map
{ local: foo; };

#--- bad.map
{ local: *; }; trailing

#--- baz.def
EXPORTS
  baz

#--- syms.s
.globl foo, bar, baz, kept, _Z3quxv, _ZN2ns1fEv, _Z1hv
foo:
bar:
baz:
kept:
_Z3quxv:
_ZN2ns1fEv:
_Z1hv:
  ret

#--- directives.s
.section .drectve
.ascii " -export:foo -export:bar -export:baz -export:kept"
.ascii " -export:_Z3quxv -export:_ZN2ns1fEv -export:_Z1hv"
