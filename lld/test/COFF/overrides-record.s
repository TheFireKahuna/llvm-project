# REQUIRES: x86, aarch64

## An overridable default states its slot in the override table and its
## forwarding as a link fact. The import library of a DLL that defines and
## exports such defaults states them in an Overrides group of its link
## records, each with the type ID that its type prefix holds, under the
## prefix key of the DLL's objects, and the library of a DLL that imports
## from it states them again, for a link that reads that library alone. A
## name has one slot and one forwarding, a slot one name, two libraries state
## one type ID for it, and a default forwards to a default that a link states
## and to itself by no path.

# RUN: rm -rf %t && split-file %s %t && cd %t

# DEFINE: %{a} = x64
# DEFINE: %{t} = x86_64
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=%{t}-windows-itanium
# DEFINE: %{dll} = lld-link -machine:%{a} -dll -noentry
# DEFINE: %{records} = llvm-readobj --coff-link-records
# DEFINE: %{run} = \
# DEFINE:   %{mc} rt.s -o rt.obj && %{mc} user.s -o user.obj && \
# DEFINE:   %{mc} bad.s -o bad.obj && \
# DEFINE:   %{mc} graph.s -o graph.obj && \
# DEFINE:   %{dll} rt.obj -out:rt.dll -implib:rt.lib && \
# DEFINE:   %{records} rt.lib | FileCheck --check-prefixes=RT,ALL %s && \
# DEFINE:   %{dll} user.obj rt.lib -out:user.dll -implib:user.lib && \
# DEFINE:   %{records} user.lib | FileCheck --check-prefixes=USER,ALL %s && \
# DEFINE:   %{dll} user.obj -out:none.dll -implib:none.lib -force:unresolved \
# DEFINE:     2>&1 | FileCheck --check-prefix=WARN %s && \
# DEFINE:   %{records} none.lib | FileCheck --check-prefix=NONE %s && \
# DEFINE:   not %{dll} rt.obj bad.obj -out:bad.dll 2>&1 \
# DEFINE:     | FileCheck --check-prefix=BAD %s && \
# DEFINE:   not %{dll} user.obj bad.obj rt.lib -out:bad.dll 2>&1 \
# DEFINE:     | FileCheck --check-prefix=LIB %s && \
# DEFINE:   %{mc} --defsym=OTHER=1 rt.s -o rt2.obj && \
# DEFINE:   %{dll} rt2.obj -out:rt2.dll -implib:rt2.lib && \
# DEFINE:   %{mc} --defsym=OTHER=1 user.s -o user2.obj && \
# DEFINE:   not %{dll} user2.obj rt.lib rt2.lib -out:bad.dll 2>&1 \
# DEFINE:     | FileCheck --check-prefix=PREFIX %s && \
# DEFINE:   not %{dll} graph.obj -out:bad.dll 2>&1 \
# DEFINE:     | FileCheck --check-prefix=GRAPH %s

# RUN: %{run}
# REDEFINE: %{a} = arm64
# REDEFINE: %{t} = aarch64
# RUN: %{run}

## A default that the DLL does not export is none that another image
## replaces.
# RT:        Kind: Overrides (0x6)
# USER:      Name: user.dll
# USER:      Kind: Overrides (0x6)
# ALL-NEXT:    PrefixKey: 1C5A06.0
# ALL-NEXT:    Default {
# ALL-NEXT:      Name: new
# ALL-NEXT:      Slot: 0
# ALL-NEXT:      TypeId: 0x33333333
# ALL-NEXT:    }
# ALL-NEXT:    Default {
# ALL-NEXT:      Name: newa
# ALL-NEXT:      Slot: 1
# ALL-NEXT:      Forward: 0
# ALL-NEXT:    }
# ALL-NEXT:    Default {
# ALL-NEXT:      Name: dela
# ALL-NEXT:      Slot: 8
# ALL-NEXT:    }
# ALL-NEXT:    Default {
# ALL-NEXT:      Name: delsa
# ALL-NEXT:      Slot: 31
# ALL-NEXT:      ForwardDropsMiddle: 8
# ALL-NEXT:    }
# ALL-NEXT:  }

# WARN: undefined symbol: plain
# NONE-NOT: Overrides

# BAD:      error: bad.obj: overridable default other of slot 0 is not stated as new of slot 0 is
# BAD-NEXT: error: bad.obj: overridable default newa of slot 4 is not stated as newa of slot 1 is
# BAD-NEXT: error: bad.obj: overridable default dela of slot 8 is not stated as dela of slot 8 is

# PREFIX: error: rt2.lib: overridable default new of slot 0 is not stated as new of slot 0 is

# GRAPH:      error: overridable default lost forwards to slot 6, which no default has
# GRAPH-NEXT: error: overridable default round forwards to itself
# GRAPH-NEXT: error: overridable default about forwards to itself

# LIB: error: rt.lib: overridable default new of slot 0 is not stated as other of slot 0 is

#--- rt.s
        .section .drectve,"yn"
        .ascii " -failifmismatch:_WIN_ITANIUM_KCFI_PREFIX=1C5A06.0"
        .ascii " -export:new -export:newa -export:dela -export:delsa"
        .ascii " -export:plain"
        .text
        .globl plain
plain:
        ret
        .weak new
__cfi_new:
.ifdef OTHER
        .long 0x11111111, 0x22222222, 0x44444444
.else
        .long 0x11111111, 0x22222222, 0x33333333
.endif
new:
        ret
.ifdef OTHER
        .globl only
only:
        ret
        .section .drectve,"yn"
        .ascii " -export:only"
        .text
.endif
        .weak newa
newa:
        ret
        .weak dela
dela:
        ret
        .weak delsa
delsa:
        ret
        .weak kept
kept:
        ret
        .link_fact new, 0, 0
        .link_fact new, 9, 0
        .link_fact newa, 9, 1, 0
        .link_fact dela, 9, 8
        .link_fact delsa, 9, 31, 8, 2
        .link_fact kept, 9, 5

#--- user.s
        .data
        .globl user
user:
        .quad plain
.ifdef OTHER
        .quad only
.endif

#--- bad.s
        .text
        .weak other
other:
        ret
        .weak newa
        .weak dela
        .link_fact other, 9, 0
        .link_fact newa, 9, 4
        .link_fact dela, 9, 8, 1

#--- graph.s
        .text
        .weak lost
lost:
        ret
        .weak round
round:
        ret
        .weak about
about:
        ret
        .link_fact lost, 9, 7, 6
        .link_fact round, 9, 10, 11
        .link_fact about, 9, 11, 10
