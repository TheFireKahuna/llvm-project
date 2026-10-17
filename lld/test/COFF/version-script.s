# REQUIRES: x86
## A GNU version script narrows the exports that objects' directives and
## -export-all-symbols give: a symbol its local: patterns match is not
## exported, one its global: patterns match is, and an unmatched one keeps its
## export. A name decides before a wildcard, the first node that names a symbol
## decides for it, and "*" decides last. extern "C++" patterns match demangled
## names. A script cannot export what nothing else does, so a global name
## that names no export is a warning. Explicit /export: options are kept.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-gnu lib.s -o lib.obj

# RUN: lld-link -dll -noentry lib.obj -out:anon.dll -version-script:anon.map \
# RUN:   2>&1 | FileCheck --check-prefix=WARN %s
# RUN: llvm-readobj --coff-exports anon.dll | FileCheck --check-prefix=ANON %s
# WARN: warning: version script assigns missing to a version, but no symbol of that name is exported
# ANON:     Name: _ZN2ns4keepEv
# ANON-NEXT: RVA
# ANON:     Name: api_one
# ANON-NOT: Name:

# RUN: lld-link -dll -noentry lib.obj -out:nodes.dll -version-script:nodes.map \
# RUN:   -export:internal_helper
# RUN: llvm-readobj --coff-exports nodes.dll | FileCheck --check-prefix=NODES %s
# NODES:     Name: _ZN2ns4dropEv
# NODES:     Name: api_one
# NODES:     Name: api_two
# NODES:     Name: internal_helper
# NODES-NOT: Name:

## -export-all-symbols, which exports every symbol of an object with no
## directives.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-gnu plain.s -o plain.obj
# RUN: lld-link -lldmingw -dll -noentry plain.obj -out:all.dll \
# RUN:   -export-all-symbols -version-script:anon.map 2>&1 | \
# RUN:   FileCheck --check-prefix=WARN %s
# RUN: llvm-readobj --coff-exports all.dll | FileCheck --check-prefix=ANON %s

# RUN: not lld-link -dll -noentry lib.obj -out:bad.dll -version-script:bad.map \
# RUN:   2>&1 | FileCheck --check-prefix=BAD %s
# BAD:     bad.map:1: ; expected, but got local
# BAD-NOT: error

#--- lib.s
        .text
        .globl api_one, api_two, internal_helper, _ZN2ns4keepEv, _ZN2ns4dropEv
api_one:
api_two:
internal_helper:
_ZN2ns4keepEv:
_ZN2ns4dropEv:
        retq
        .section .drectve,"yni"
        .ascii " -export:api_one -export:api_two -export:internal_helper"
        .ascii " -export:_ZN2ns4keepEv -export:_ZN2ns4dropEv"

#--- plain.s
        .text
        .globl api_one, api_two, internal_helper, _ZN2ns4keepEv, _ZN2ns4dropEv
api_one:
api_two:
internal_helper:
_ZN2ns4keepEv:
_ZN2ns4dropEv:
        retq

#--- anon.map
/* Only the API and ns::keep stay exported. */
{
  global:
    api_one;   # an exact name
    missing;
    extern "C++" {
      "ns::keep()";
    };
  local: *;
};

#--- nodes.map
V1 {
  global: api_*;
  local: internal_*; _ZN2ns4keepEv;
};
V2 {
  global: _ZN2ns4keepEv; extern "C++" { ns::d* };
} V1;

#--- bad.map
{ global: api_one local: *; };
