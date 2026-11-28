# REQUIRES: x86, aarch64

## A word of data that holds an address inside imported data for which the
## import's DLL exports no name is written by the image's fill, in the
## runtime, as the image starts. The word holds 0xF1F1F1F1F1F1F1F1, which
## no pointer holds, until then, and has no base
## relocation, and the fill's list names it with the import's entry of its
## address table, which the import keeps, and the offset. A section of .rdata
## that holds such a word leaves for .sealed, read-only, which begins with the
## word that the fill sets once it has written; an in-place import slot of
## such a section is inside the import address table directory. A read-only
## section of another name keeps its place and the list names a range for it,
## and a word of writable data has neither. The image's entry point is the
## runtime's __llvm_fill_dll or __llvm_fill_exe, and is the only one of the two
## in the guard function table; its __llvm_fill_target is the entry the image
## was linked with, or __llvm_fill_true in a DLL with none. An image whose
## words are all writable has no .sealed.
##
## __imp_X$val names a word of the image's own that the fill writes with the
## word that import X holds, and __imp_X$init one that it writes with what X
## returns for the image's base. Each is a word of .sealed that holds zero and
## has no base relocation, and code reads it where it is. They follow the word
## that .sealed begins with, every $init and then every $val, each in the
## order of their names, and so does the list, which states a word's kind in
## the low bits of the address of its import's entry: 2 for $init and 1 for
## $val. The import keeps that entry though nothing else names it. A value
## that only a discarded section reads is no part of the image.

# RUN: rm -rf %t && split-file %s %t && cd %t

# DEFINE: %{a} = x64
# DEFINE: %{t} = x86_64
# DEFINE: %{p} = X64
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=%{t}-windows-itanium
# DEFINE: %{link} = lld-link -machine:%{a} imp.lib ntdll.lib rt.lib
# DEFINE: %{np} = llvm-mc -filetype=obj -triple=%{t}-windows-ntposix
# DEFINE: %{nplink} = lld-link -machine:%{a} -subsystem:console imp.lib \
# DEFINE:   ntdll.lib rtn.lib np.lib
# DEFINE: %{dump} = llvm-readobj --file-headers --sections --coff-imports \
# DEFINE:   --coff-basereloc --coff-load-config --unwind
# DEFINE: %{run} = \
# DEFINE:   lld-link -machine:%{a} -def:imp.def -out:imp.lib && \
# DEFINE:   lld-link -machine:%{a} -def:ntdll.def -out:ntdll.lib && \
# DEFINE:   %{mc} data.s -o data.obj && %{mc} slot.s -o slot.obj && \
# DEFINE:   %{mc} rw.s -o rw.obj && \
# DEFINE:   %{mc} cfg.s -o cfg.obj && %{mc} --defsym %{a}=1 rt.s -o rt.obj && \
# DEFINE:   rm -f rt.lib && llvm-ar rcs rt.lib rt.obj && \
# DEFINE:   %{link} -dll -entry:start -guard:cf data.obj cfg.obj \
# DEFINE:     -out:fill.dll && \
# DEFINE:   %{dump} fill.dll > fill.txt && \
# DEFINE:   llvm-objdump -s -j .text -j .sealed -j .rdata -j .rgrp -j .data \
# DEFINE:     fill.dll >> fill.txt && \
# DEFINE:   FileCheck --check-prefixes=CHECK,%{p} %s < fill.txt && \
# DEFINE:   %{link} -dll -noentry data.obj -out:noentry.dll && \
# DEFINE:   llvm-readobj --file-headers noentry.dll > noentry.txt && \
# DEFINE:   llvm-objdump -s -j .text noentry.dll >> noentry.txt && \
# DEFINE:   FileCheck --check-prefixes=NOENTRY,NOENTRY-%{p} %s < noentry.txt && \
# DEFINE:   %{link} -subsystem:console -entry:start data.obj -out:fill.exe && \
# DEFINE:   %{dump} fill.exe > exe.txt && \
# DEFINE:   llvm-objdump -s -j .text fill.exe >> exe.txt && \
# DEFINE:   FileCheck --check-prefixes=EXE,EXE-%{p} %s < exe.txt && \
# DEFINE:   %{link} -dll -entry:start rw.obj -out:rw.dll && \
# DEFINE:   llvm-readobj --sections rw.dll > rw.txt && \
# DEFINE:   llvm-objdump -s -j .rdata rw.dll >> rw.txt && \
# DEFINE:   FileCheck --check-prefix=RW %s < rw.txt && \
# DEFINE:   lld-link -machine:%{a} -def:val.def -out:val.lib && \
# DEFINE:   %{mc} --defsym %{a}=1 val.s -o val.obj && \
# DEFINE:   %{link} val.lib -dll -noentry val.obj -out:sealed.dll && \
# DEFINE:   llvm-readobj --sections --coff-imports --coff-basereloc sealed.dll \
# DEFINE:     > val.txt && \
# DEFINE:   llvm-objdump -d --no-show-raw-insn sealed.dll >> val.txt && \
# DEFINE:   llvm-objdump -s -j .sealed -j .rdata sealed.dll >> val.txt && \
# DEFINE:   FileCheck --check-prefixes=VAL,VAL-%{p} %s < val.txt && \
# DEFINE:   llvm-lib -machine:%{a} -def:np.def -out:np.lib && \
# DEFINE:   %{np} --defsym %{a}=1 rt.s -o rtn.obj && \
# DEFINE:   rm -f rtn.lib && llvm-ar rcs rtn.lib rtn.obj && \
# DEFINE:   %{np} np.s -o np.obj && %{np} np.s --defsym ODD=1 -o npo.obj && \
# DEFINE:   %{nplink} -entry:start np.obj -out:np.exe && \
# DEFINE:   llvm-objdump -s -j .rdata -j .tls np.exe > np.txt && \
# DEFINE:   FileCheck --check-prefix=NP %s < np.txt && \
# DEFINE:   %{link} -dll -entry:start slot.obj -out:nofill.dll && \
# DEFINE:   %{dump} nofill.dll | FileCheck --check-prefixes=NOFILL,GC %s && \
# DEFINE:   %{link} -dll -entry:start -opt:noref slot.obj -out:noref.dll && \
# DEFINE:   %{dump} noref.dll | FileCheck --check-prefix=NOFILL %s

# RUN: %{run}
# REDEFINE: %{a} = arm64
# REDEFINE: %{t} = aarch64
# REDEFINE: %{p} = A64
# RUN: %{run}

# CHECK:      AddressOfEntryPoint: 0x1010
## The directory begins with .sealed, which precedes .rdata and its tables.
# CHECK:      IATRVA: 0x2000
# CHECK-NEXT: IATSize: 0x1028
# CHECK:      Name: .text
# CHECK:      Name: .sealed
# CHECK-NEXT: VirtualSize: 0x20
# CHECK-NEXT: VirtualAddress: 0x2000
# CHECK:      Characteristics [ (0x40000040)
# CHECK:      Name: .rdata
# CHECK:      Name: .rgrp
# CHECK-NEXT: VirtualSize: 0x18
# CHECK-NEXT: VirtualAddress: 0x5000
## The fill's own import, the entries that the words are computed from, and
## the slot of q.
# CHECK:      Name: ntdll.dll
# CHECK:      Symbol: NtProtectVirtualMemory (
# CHECK-NEXT: }
# CHECK:      Name: imp.dll
# CHECK:      ImportAddressTableRVA: 0x3010
# CHECK-NEXT: Symbol: obj (
# CHECK-NEXT: Symbol: other (
# CHECK-NEXT: }
# CHECK:      Name: imp.dll
# CHECK:      ImportAddressTableRVA: 0x2010
# CHECK-NEXT: Symbol: other (
# CHECK-NEXT: }
## Only the load configuration's pointer is adjusted.
# CHECK:      BaseReloc [
# CHECK-NEXT:   Entry {
# CHECK-NEXT:     Type: DIR64
# CHECK-NEXT:     Address: 0x30B0
# CHECK-NEXT:   }
# CHECK-NEXT:   Entry {
# CHECK-NEXT:     Type: ABSOLUTE
# CHECK:      GuardFidTable [
# CHECK-NEXT:   0x180001010
# CHECK-NEXT: ]

## The entry's __llvm_fill_target is start.
# CHECK:      Contents of section .text:
# X64:         180001010 {{.*}} 00001000
# X64-NEXT:    180001020 00
# A64:         180001010 {{.*}} 00100000

## The done word, p, and q with its slot.
# CHECK:      Contents of section .sealed:
# CHECK-NEXT:  180002000 00000000 00000000 f1f1f1f1 f1f1f1f1
# CHECK-NEXT:  180002010 {{[0-9a-f]+}}0000 00000000 f1f1f1f1 f1f1f1f1
## k stays. The list: one range, five words, .sealed; .rgrp's range; and the
## words in the order of their addresses.
# CHECK:      Contents of section .rdata:
# CHECK:       180003020 00000000 00000000 07000000 00000000
# CHECK:       180003130 01000000 00000000 05000000 00000000
# CHECK-NEXT:  180003140 00200000 20000000 00000000 00000000
# CHECK-NEXT:  180003150 08500000 10000000 08200000 10300000
# CHECK-NEXT:  180003160 18000000 00000000 18200000 10300000
# CHECK-NEXT:  180003170 f8ffffff ffffffff 00400000 10300000
# CHECK-NEXT:  180003180 38000000 00000000 08500000 10300000
# CHECK-NEXT:  180003190 28000000 00000000 10500000 18300000
# CHECK-NEXT:  1800031a0 30000000 00000000
# CHECK:      Contents of section .data:
# CHECK-NEXT:  180004000 f1f1f1f1 f1f1f1f1
# CHECK:      Contents of section .rgrp:
# CHECK-NEXT:  180005000 55000000 00000000 f1f1f1f1 f1f1f1f1
# CHECK-NEXT:  180005010 f1f1f1f1 f1f1f1f1

## A DLL with no entry has __llvm_fill_true for one, and an executable's
## entry is __llvm_fill_exe.
# NOENTRY:      AddressOfEntryPoint: 0x1010
# NOENTRY-X64:  180001010 {{.*}} 00211000
# NOENTRY-X64:  180001020 00c3
# NOENTRY-A64:  180001010 {{.*}} 20100000
# NOENTRY-A64:  180001020 c0035fd6
# EXE:          AddressOfEntryPoint: 0x1010
# EXE-X64:      140001010 {{.*}} 00001000
# EXE-A64:      140001010 {{.*}} 00100000

## The fill stores a word of writable data where it is: an image with no
## other has no .sealed, which its list says.
# RW-NOT: Name: .sealed
# RW:      180002020 00000000 00000000 01000000 00000000
# RW-NEXT: 180002030 00000000 00000000 00000000 00000000
# RW-NEXT: 180002040 00300000 10200000 08000000 00000000

# VAL:          Name: .sealed
# VAL-NEXT:     VirtualSize: 0x20
# VAL-NEXT:     VirtualAddress: 0x3000
# VAL:          Characteristics [ (0x40000040)
# VAL:          Name: val.dll
# VAL:          ImportAddressTableRVA: 0x2010
# VAL-NEXT:     Symbol: attach (
# VAL-NEXT:     Symbol: base (
# VAL-NEXT:     Symbol: map (
# VAL-NEXT:     }
# VAL:          BaseReloc [
# VAL-NEXT:     ]
# VAL-X64:      <.text>:
# VAL-X64-NEXT: movq {{.*}} # 0x180003018
# VAL-X64-NEXT: movq {{.*}} # 0x180003010
# VAL-X64-NEXT: movq {{.*}} # 0x180003008
# VAL-A64:      <.text>:
# VAL-A64-NEXT: ldr x0, 0x180003018
# VAL-A64-NEXT: ldr x1, 0x180003010
# VAL-A64-NEXT: ldr x2, 0x180003008
# VAL:          Contents of section .rdata:
# VAL:           180002030 00000000 00000000 03000000 00000000
# VAL-NEXT:      180002040 00300000 20000000 00000000 00000000
# VAL-NEXT:      180002050 08300000 12200000 00000000 00000000
# VAL-NEXT:      180002060 10300000 19200000 00000000 00000000
# VAL-NEXT:      180002070 18300000 21200000 00000000 00000000
# VAL:          Contents of section .sealed:
# VAL-NEXT:      180003000 00000000 00000000 00000000 00000000
# VAL-NEXT:      180003010 00000000 00000000 00000000 00000000

## In the ntposix model the template is read-only and the runtime copies it:
## a word there is one of a range the fill opens, written before the
## executable's fill registers the template again, and only there. A word at
## an offset from the template's start that is not a multiple of 8 bytes is in
## error, and so is an executable that would not register it again. The place
## that it registers through comes first, and holds the executable's place.
# NP:      140002030 01000000 00000000 02000000 00000000
# NP-NEXT: 140002040 00300000 18000000 00000000 00000000
# NP-NEXT: 140002050 00400000 08000000 08300000 22200000
# NP-NEXT: 140002060 00000000 00000000 00400000 10200000
# NP-NEXT: 140002070 08000000 00000000
# NP:      Contents of section .tls:
# NP-NEXT: 140004000 f1f1f1f1 f1f1f1f1
# RUN: not lld-link -machine:%{a} -subsystem:console imp.lib ntdll.lib rtn.lib \
# RUN:   -entry:start np.obj -out:bad.exe 2>&1 | \
# RUN:   FileCheck --check-prefix=NP-REG %s
# RUN: not %{nplink} -dll -entry:start npo.obj -out:bad.dll 2>&1 | \
# RUN:   FileCheck --check-prefix=NP-ODD %s
# NP-REG: error: np.obj: .tls$+0x0: holds an address inside obj, for which imp.dll exports no name, in the thread-local template of an executable, which its fill registers again through __imp___tp_register$init, which the image does not have
# NP-ODD: error: npo.obj: .tls$+0x4: holds an address inside obj, for which imp.dll exports no name, at an offset from the start of the thread-local template that is not a multiple of 8 bytes

## An image that has no word for the fill has none of this, though a library
## offers the function: its entry is its own, and where the link collects
## unreferenced sections it imports nothing for the fill.
# NOFILL:     AddressOfEntryPoint: 0x1000
# NOFILL:     ExceptionTableRVA: 0x0
# NOFILL-NOT: Name: .sealed
# GC-NOT:     Name: ntdll.dll

## The fill is the runtime's, and only the image's header names its entry. It
## gives pages a protection, so .sealed keeps to itself and stays read-only, a
## section that it writes in holds no code, and sections begin on pages. A
## word of thread-local data reaches no thread that copied it first.
# RUN: not lld-link -machine:arm64 -dll -noentry imp.lib ntdll.lib data.obj \
# RUN:   -out:bad.dll 2>&1 | FileCheck --check-prefix=NORT %s
# NORT: error: data.obj: .data+0x0: holds an address inside obj, for which imp.dll exports no name: the image computes it as it starts, which needs __llvm_fill_dll; link the builtins library
# RUN: not lld-link -machine:arm64 -dll -noentry val.lib ntdll.lib val.obj \
# RUN:   -out:bad.dll 2>&1 | FileCheck --check-prefix=VAL-NORT %s
# VAL-NORT: error: the image reads a word that it writes from attach: the image computes it as it starts, which needs __llvm_fill_dll; link the builtins library

## An image that names a value for an import that it lacks is in error.
# RUN: not %{link} -dll -noentry val.obj -out:bad.dll 2>&1 | \
# RUN:   FileCheck --check-prefix=ABSENT %s
# ABSENT: error: undefined symbol: __declspec(dllimport) map$val

## An image with TLS callbacks of its own runs the fill as the first of them,
## __llvm_fill_tls, which .CRT$XLAF holds, since the loader calls them before
## the entry.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium --defsym x64=1 \
# RUN:   rt.s -o rt64.obj
# RUN: rm -f rt64.lib && llvm-ar rcs rt64.lib rt64.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium callback.s \
# RUN:   -o callback.obj
# RUN: lld-link -machine:x64 -def:val.def -out:val64.lib
# RUN: lld-link -machine:x64 -def:ntdll.def -out:ntdll64.lib
# RUN: lld-link -machine:x64 -dll -noentry val64.lib ntdll64.lib rt64.lib \
# RUN:   callback.obj -out:callback.dll -map:callback.map
# RUN: FileCheck --check-prefix=CALLBACK %s < callback.map
# CALLBACK:      .CRT$XLAF
# CALLBACK-NEXT: .CRT$XLB

## An executable with a TLS directory runs it there whatever callbacks it has,
## since its callbacks run before any thread that a DLL starts as the process
## starts; a DLL with none of its own runs it from its entry.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium tlsdir.s \
# RUN:   -o tlsdir.obj
# RUN: lld-link -machine:x64 -entry:f -subsystem:console val64.lib \
# RUN:   ntdll64.lib rt64.lib tlsdir.obj -out:tlsdir.exe -map:tlsdir.map
# RUN: FileCheck --check-prefix=TLSDIR %s < tlsdir.map
# RUN: lld-link -machine:x64 -dll -noentry val64.lib ntdll64.lib rt64.lib \
# RUN:   tlsdir.obj -out:tlsdir.dll -map:tlsdir-dll.map
# RUN: FileCheck --check-prefix=TLSDIR-DLL %s < tlsdir-dll.map
# TLSDIR:         .CRT$XLAF
# TLSDIR-DLL-NOT: .CRT$XLAF

## A link that collects no garbage holds only the callback the link selects.
# RUN: lld-link -machine:x64 -entry:f -subsystem:console -opt:noref val64.lib \
# RUN:   ntdll64.lib rt64.lib tlsdir.obj -out:tlsdir.exe -map:noref.map
# RUN: FileCheck --check-prefix=NOREF %s < noref.map
# NOREF: 00000008H .CRT$XLAF

## A link that collects no garbage holds only the callback the link selects.
# RUN: lld-link -machine:x64 -entry:f -subsystem:console -opt:noref val64.lib \
# RUN:   ntdll64.lib rt64.lib tlsdir.obj -out:tlsdir.exe -map:noref.map
# RUN: FileCheck --check-prefix=NOREF %s < noref.map
# NOREF: 00000008H .CRT$XLAF

## The value of a word the image defines, as the runtime's words are under
## -static, is that word, which code reads with nothing for the fill to write.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium own.s -o own.obj
# RUN: lld-link -machine:x64 -dll -noentry -include:f own.obj -out:own.dll
# RUN: llvm-readobj --sections own.dll > own.txt
# RUN: llvm-objdump -d --no-show-raw-insn own.dll >> own.txt
# RUN: FileCheck --check-prefix=OWN %s < own.txt
# OWN-NOT:  Name: .sealed
# OWN:      Name: .data
# OWN-NEXT: VirtualSize:
# OWN-NEXT: VirtualAddress: 0x3000
# OWN:      movq 0x1ff9(%rip), %rax # 0x180003000

## So is an archive member's that defines it, which the value asks for.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium map.s -o map.obj
# RUN: rm -f map.lib && llvm-ar rcs map.lib map.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium reads.s -o reads.obj
# RUN: lld-link -machine:x64 -dll -noentry -include:f reads.obj map.lib \
# RUN:   -out:member.dll
# RUN: llvm-readobj --sections member.dll > member.txt
# RUN: llvm-objdump -d --no-show-raw-insn member.dll >> member.txt
# RUN: FileCheck --check-prefix=OWN %s < member.txt

# RUN: %{mc} bad.s -o bad.obj
# RUN: not %{link} -dll -noentry data.obj bad.obj -merge:.rgrp=.text \
# RUN:   -merge:.sealed=.rdata -section:.sealed,rw -align:512 -out:bad.dll \
# RUN:   2>&1 | FileCheck --check-prefix=BAD %s
# BAD: error: bad.obj: .tls$+0x0: holds an address inside obj, for which imp.dll exports no name, in thread-local data; set the variable when the program starts
# BAD: error: bad.obj: .tls$r+0x0: holds an address inside obj, for which imp.dll exports no name, in thread-local data; set the variable when the program starts
# BAD: error: bad.obj: refers to __llvm_fill_dll, which the loader calls, and nothing else may name
# BAD: error: /merge: cannot merge '.sealed' with any section: it holds what the image writes as it starts and nothing else
# BAD: error: data.obj: .rgrp$m+0x8: holds an address inside obj, for which imp.dll exports no name, in the executable section .text, where the image cannot write it as it starts; keep its section apart from /merge and /section
# BAD: error: /section: .sealed is read-only data, which the image writes as it starts and no later
# BAD: error: /align:512: the image writes read-only data as it starts, a page at a time, so its sections are aligned to 4096 bytes or more

## A hybrid image has no fill.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium data.s -o x64.obj
# RUN: llvm-lib -machine:arm64ec -def:imp.def -out:imp-ec.lib
# RUN: not lld-link -machine:arm64ec -dll -noentry imp-ec.lib x64.obj \
# RUN:   -out:bad.dll 2>&1 | FileCheck --check-prefix=EC %s
# EC: error: x64.obj: .data holds the address of an import, __imp_obj, in a word of data, which the loader binds in place only in an x64 or ARM64 image

#--- imp.def
LIBRARY imp.dll
EXPORTS
obj DATA
other DATA

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
LdrGetProcedureAddressForCaller
NtProtectVirtualMemory
RtlExitUserProcess

#--- data.s
.text
.globl start
start:
  ret
.section .rdata,"dr",one_only,p
.globl p
p:
  .quad obj+24
.section .rdata,"dr",one_only,q
.globl q
q:
  .quad other
  .quad obj-8
.section .rdata,"dr",one_only,k
.globl k
k:
  .quad 7
.section .rgrp$m,"dr"
  .quad 0x55
  .quad obj+40
  .quad other+48
.data
  .quad obj+56
.section .drectve,"yn"
  .ascii " -include:p -include:q -include:k"

#--- slot.s
.text
.globl start
start:
  ret
.data
  .quad obj

#--- rw.s
.text
.globl start
start:
  ret
.data
  .quad obj+8

#--- val.def
LIBRARY val.dll
EXPORTS
attach
base DATA
gone DATA
map DATA

#--- val.s
.text
.globl f
f:
.ifdef x64
  movq __imp_map$val(%rip), %rax
  movq __imp_base$val(%rip), %rcx
  movq __imp_attach$init(%rip), %rdx
  retq
.else
  adrp x0, __imp_map$val
  ldr x0, [x0, :lo12:__imp_map$val]
  adrp x1, __imp_base$val
  ldr x1, [x1, :lo12:__imp_base$val]
  adrp x2, __imp_attach$init
  ldr x2, [x2, :lo12:__imp_attach$init]
  ret
.endif
.section .text,"xr",one_only,dead
dead:
  .quad __imp_gone$val
.section .drectve,"yn"
  .ascii " -include:f"

#--- callback.s
.text
.globl f
f:
  movq __imp_map$val(%rip), %rax
  retq
callback:
  retq
.section .CRT$XLB,"dr"
  .quad callback
.section .drectve,"yn"
  .ascii " -include:f"

#--- tlsdir.s
.text
.globl f
f:
  movq __imp_map$val(%rip), %rax
  retq
.section .rdata,"dr"
.globl _tls_used
_tls_used:
  .zero 40
.section .drectve,"yn"
  .ascii " -include:f -include:_tls_used"

#--- own.s
.data
.globl map
map:
  .quad 0
.text
.globl f
f:
  movq __imp_map$val(%rip), %rax
  retq

#--- map.s
.data
.globl map
map:
  .quad 0

#--- reads.s
.text
.globl f
f:
  movq __imp_map$val(%rip), %rax
  retq

#--- np.def
LIBRARY ntposix.dll
EXPORTS
__tp_register

#--- np.s
.text
.globl start
start:
  ret
.section .tls$,"dw"
.ifdef ODD
  .long 0
.endif
  .quad obj+8

#--- bad.s
.data
  .quad __llvm_fill_dll
.section .tls$,"dw"
  .quad obj+8
.section .tls$r,"dr"
  .quad obj+16

#--- rt.s
.macro entry name, import
.section .text,"xr",one_only,\name
.globl \name
.def \name
.type 32
.endef
.p2align 4
\name:
.ifdef x64
  leaq __llvm_fill_list(%rip), %rax
  jmpq *__imp_\import(%rip)
.else
  adrp x0, __llvm_fill_list
  adrp x16, __imp_\import
  ldr x16, [x16, :lo12:__imp_\import]
  br x16
.endif
  .rva __llvm_fill_target
.endm
  entry __llvm_fill_dll, NtProtectVirtualMemory
  entry __llvm_fill_exe, NtProtectVirtualMemory
  entry __llvm_fill_dll_imports, LdrGetProcedureAddressForCaller
  entry __llvm_fill_exe_imports, LdrGetProcedureAddressForCaller
.section .text,"xr",one_only,__llvm_fill_true
.globl __llvm_fill_true
__llvm_fill_true:
  ret
.section .text,"xr",one_only,__llvm_fill_tls
.globl __llvm_fill_tls
__llvm_fill_tls:
  ret
.section .CRT$XLAF,"dr",associative,__llvm_fill_tls
  .quad __llvm_fill_tls
.section .text,"xr",one_only,__llvm_fill_imports_tls
.globl __llvm_fill_imports_tls
__llvm_fill_imports_tls:
  ret
.section .CRT$XLAF,"dr",associative,__llvm_fill_imports_tls
  .quad __llvm_fill_imports_tls

#--- cfg.s
.section .rdata,"dr"
.p2align 3
.globl _load_config_used
_load_config_used:
  .long 256
  .fill 124, 1, 0
  .quad __guard_fids_table
  .quad __guard_fids_count
  .long __guard_flags
  .fill 108, 1, 0
