# REQUIRES: x86, aarch64

## Beside a fork object, a data word holding an import's address is an in-place
## slot (lookup entry, no base relocation, a descriptor's address table);
## delay-loaded and debug words are left, code and overlong words are errors.

# RUN: rm -rf %t && split-file %s %t && cd %t

# DEFINE: %{a} = x64
# DEFINE: %{t} = x86_64
# DEFINE: %{m} = IMAGE_FILE_MACHINE_AMD64
# DEFINE: %{r} = IMAGE_REL_AMD64_ADDR64
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=%{t}-windows
# DEFINE: %{link} = lld-link -machine:%{a} -dll -noentry imp.lib
# DEFINE: %{run} = lld-link -machine:%{a} -def:imp.def -out:imp.lib && \
# DEFINE:   %{mc}-itanium data.s -o data.obj && \
# DEFINE:   %{mc}-itanium var.s -o var.obj && \
# DEFINE:   %{mc}-msvc data.s -o foreign.obj && \
# DEFINE:   %{mc}-msvc local.s -o local.obj && \
# DEFINE:   %{mc}-itanium code.s -o code.obj && \
# DEFINE:   yaml2obj -DMACHINE=%{m} -DTYPE=%{r} -DAT=4 past.yaml \
# DEFINE:     -o past.obj && \
# DEFINE:   yaml2obj -DMACHINE=%{m} -DTYPE=%{r} -DAT=0 past.yaml \
# DEFINE:     -o twice.obj && %{link} -out:slots.dll data.obj var.obj && \
# DEFINE:   llvm-readobj --coff-imports --coff-basereloc slots.dll > slots.txt && \
# DEFINE:   llvm-objdump -s -j .rdata -j .data slots.dll >> slots.txt && \
# DEFINE:   FileCheck --check-prefix=SLOTS %s < slots.txt && \
# DEFINE:   %{link} -out:pdb.dll data.obj var.obj -debug -pdb:pdb.pdb && \
# DEFINE:   llvm-pdbutil dump --section-headers pdb.pdb | \
# DEFINE:     FileCheck --check-prefix=PDB %s && \
# DEFINE:   llvm-readobj --coff-imports pdb.dll | \
# DEFINE:     FileCheck --check-prefix=DEBUG %s && \
# DEFINE:   %{link} -out:foreign.dll foreign.obj local.obj && \
# DEFINE:   llvm-readobj --coff-imports --coff-basereloc foreign.dll | \
# DEFINE:     FileCheck --check-prefix=FOREIGN %s && \
# DEFINE:   %{link} -out:mixed.dll foreign.obj var.obj twice.obj && \
# DEFINE:   llvm-readobj --coff-imports --coff-basereloc mixed.dll | \
# DEFINE:     FileCheck --check-prefix=MIXED %s && \
# DEFINE:   %{link} -out:delay.dll data.obj -delayload:imp.dll \
# DEFINE:     -alternatename:__delayLoadHelper2=fn2 && \
# DEFINE:   llvm-readobj --coff-imports --coff-basereloc delay.dll | \
# DEFINE:     FileCheck --check-prefix=DELAY %s && \
# DEFINE:   not %{link} -out:bad.dll code.obj past.obj 2>&1 | \
# DEFINE:     FileCheck --check-prefix=BAD %s

# RUN: %{run}
# REDEFINE: %{a} = arm64
# REDEFINE: %{t} = aarch64
# REDEFINE: %{m} = IMAGE_FILE_MACHINE_ARM64
# REDEFINE: %{r} = IMAGE_REL_ARM64_ADDR64
# RUN: %{run}

## One descriptor per run (.data's two words, .rdata's pair); no other import is
## used, so the DLL has no table of its own (import-slots-reuse.s).
# SLOTS:      Name: imp.dll
# SLOTS:      ImportAddressTableRVA: 0x2008
# SLOTS-NEXT: Symbol: fn (0)
# SLOTS-NEXT: }
# SLOTS:      Name: imp.dll
# SLOTS:      ImportAddressTableRVA: 0x2018
# SLOTS-NEXT: Symbol: fn2 (1)
# SLOTS-NEXT: }
# SLOTS:      Name: imp.dll
# SLOTS:      ImportAddressTableRVA: 0x1000
# SLOTS-NEXT: Symbol: fn (0)
# SLOTS-NEXT: Symbol: var (2)
# SLOTS-NEXT: }
# SLOTS-NOT:  Name: imp.dll
## Only the word that holds a local address is relocated.
# SLOTS:      BaseReloc [
# SLOTS-NEXT:   Entry {
# SLOTS-NEXT:     Type: DIR64
# SLOTS-NEXT:     Address: 0x2010
# SLOTS-NEXT:   }
# SLOTS-NEXT:   Entry {
# SLOTS-NEXT:     Type: ABSOLUTE
# SLOTS-NEXT:     Address:
# SLOTS-NEXT:   }
# SLOTS-NEXT: ]
## The lookup tables and the slots hold the same entries; .rdata's slots start
## the section.
# SLOTS:      Contents of section .rdata:
# SLOTS-NEXT:  180001000 [[FN:[0-9a-f]+]] 00000000 [[VAR:[0-9a-f]+]] 00000000
# SLOTS:       180001060 [[FN]] 00000000 [[VAR]] 00000000
# SLOTS-NEXT:  180001070 00000000 00000000 [[FN2:[0-9a-f]+]] 00000000
# SLOTS-NEXT:  180001080 00000000 00000000 [[FN]] 00000000
# SLOTS:      Contents of section .data:
# SLOTS-NEXT:  180002000 01000000 02000000 [[FN]] 00000000
# SLOTS-NEXT:  180002010 20200080 01000000 [[FN2]] 00000000

## The sections that follow the table leave .rdata's own group empty.
# PDB: .rdata name
## The word of the debug section, kept here, is no slot.
# DEBUG-COUNT-4: Name: imp.dll
# DEBUG-NOT:     Name: imp.dll

## Without a fork object the words hold the thunk's address, also beside a
## local import pointer.
# FOREIGN:      Name: imp.dll
# FOREIGN-NOT:  Name: imp.dll
# FOREIGN:      BaseReloc [
# FOREIGN:        Address: 0x3008
# FOREIGN:        Address: 0x3010
# FOREIGN:        Address: 0x3018
# FOREIGN:        Address: 0x3028
# FOREIGN:        Address: 0x3030

## With one, other objects' words are slots too, one of them named by two
## relocations.
# MIXED:      Name: imp.dll
# MIXED:      Name: imp.dll
# MIXED:      Name: imp.dll
# MIXED:      Name: imp.dll
# MIXED-NOT:  Name: imp.dll
# MIXED:      BaseReloc [
# MIXED-NEXT:   Entry {
# MIXED-NEXT:     Type: DIR64
# MIXED-NEXT:     Address: 0x2010
# MIXED-NEXT:   }
# MIXED-NEXT:   Entry {
# MIXED-NEXT:     Type: ABSOLUTE

## A delay-loaded function's words keep the thunk and their relocations.
# DELAY-NOT:  Name: imp.dll
# DELAY:      DelayImport {
# DELAY:      Address: 0x3008
# DELAY:      Address: 0x3010
# DELAY:      Address: 0x3018

# BAD-DAG: error: code.obj: .text+0x0: {{.*}} holds the address of fn, imported from imp.dll
# BAD-DAG: error: past.obj: .rdata+0x4: {{.*}} holds the address of fn, imported from imp.dll

## A hybrid image has no slots: the first object that holds one is reported.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium data.s -o x64.obj
# RUN: llvm-mc -filetype=obj -triple=arm64ec-windows \
# RUN:   %S/Inputs/loadconfig-arm64ec.s -o loadconfig.obj
# RUN: llvm-mc -filetype=obj -triple=arm64ec-windows icall.s -o icall.obj
# RUN: llvm-lib -machine:arm64ec -def:imp.def -out:imp-ec.lib
# RUN: not lld-link -machine:arm64ec -dll -noentry imp-ec.lib x64.obj icall.obj \
# RUN:   loadconfig.obj -out:ec.dll 2>&1 | FileCheck --check-prefix=EC %s
# EC: error: x64.obj: .data holds the address of an import, __imp_aux_fn, in a word of data
# EC-NOT: error:

#--- imp.def
LIBRARY imp.dll
EXPORTS
fn
fn2
var DATA

#--- data.s
.section .rdata,"dr"
.globl ro
ro:
  .quad fn
.data
.globl rw
rw:
  .long 1
  .long 2
  .quad fn
  .quad local
  .quad fn2
local:
  .quad 0
.section .debug_info,"drD"
  .quad fn

#--- local.s
.data
.globl here
here:
  .quad __imp_here
  .quad fn

#--- var.s
.section .rdata,"dr"
  .quad var

#--- code.s
.text
  .quad fn

#--- icall.s
.globl __icall_helper_arm64ec
__icall_helper_arm64ec:
  ret

#--- past.yaml
--- !COFF
header:
  Machine:         [[MACHINE]]
  Characteristics: [  ]
sections:
  - Name:            .rdata
    Characteristics: [ IMAGE_SCN_CNT_INITIALIZED_DATA, IMAGE_SCN_MEM_READ ]
    Alignment:       8
    SectionData:     '0000000000000000'
    Relocations:
      - { VirtualAddress: [[AT]], SymbolName: fn, Type: [[TYPE]] }
      - { VirtualAddress: [[AT]], SymbolName: fn, Type: [[TYPE]] }
symbols:
  - { Name: .rdata, Value: 0, SectionNumber: 1, SimpleType: IMAGE_SYM_TYPE_NULL, ComplexType: IMAGE_SYM_DTYPE_NULL, StorageClass: IMAGE_SYM_CLASS_STATIC }
  - { Name: fn, Value: 0, SectionNumber: 0, SimpleType: IMAGE_SYM_TYPE_NULL, ComplexType: IMAGE_SYM_DTYPE_FUNCTION, StorageClass: IMAGE_SYM_CLASS_EXTERNAL }
...
