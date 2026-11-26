# REQUIRES: x86, aarch64

## A word holding an address inside an import binds to the name its DLL exports
## for it (X$so<n>, X$ap<n>), loading that member; with no such name from that
## DLL, or before the import, it is an error and no library object is loaded.

# RUN: rm -rf %t && split-file %s %t && cd %t

# DEFINE: %{a} = x64
# DEFINE: %{t} = x86_64
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=%{t}-windows-itanium
# DEFINE: %{link} = lld-link -machine:%{a} -dll -noentry imp.lib other.lib
# DEFINE: %{run} = lld-link -machine:%{a} -def:imp.def -out:imp.lib && \
# DEFINE:   lld-link -machine:%{a} -def:other.def -out:other.lib && \
# DEFINE:   %{mc} data.s -o data.obj && %{mc} bad.s -o bad.obj && \
# DEFINE:   %{mc} member.s -o member.obj && \
# DEFINE:   rm -f objects.lib && llvm-ar rcs objects.lib member.obj && \
# DEFINE:   %{link} data.obj -out:interior.dll -verbose 2> log.txt && \
# DEFINE:   llvm-readobj --coff-imports --coff-basereloc interior.dll \
# DEFINE:     >> log.txt && \
# DEFINE:   llvm-objdump -s -j .rdata -j .data interior.dll >> log.txt && \
# DEFINE:   FileCheck %s < log.txt && %{mc} debug.s -o debug.obj && \
# DEFINE:   %{link} data.obj debug.obj -out:debug.dll -debug -pdb:debug.pdb && \
# DEFINE:   llvm-readobj --file-headers --coff-imports debug.dll | \
# DEFINE:     FileCheck --check-prefix=DEBUG %s && \
# DEFINE:   not %{link} bad.obj objects.lib -out:bad.dll -verbose 2>&1 | \
# DEFINE:     FileCheck --check-prefix=BAD \
# DEFINE:       --implicit-check-not="Loading lazy __imp_obj$so32" %s

# RUN: %{run}
# REDEFINE: %{a} = arm64
# REDEFINE: %{t} = aarch64
# RUN: %{run}

# CHECK-DAG: Loading lazy __imp_obj$so8 from imp.lib for an address inside __imp_obj
# CHECK-DAG: Loading lazy __imp_vt$ap16 from imp.lib for an address inside __imp_vt
# CHECK:      Name: imp.dll
# CHECK:      Symbol: obj (
# CHECK-NEXT: Symbol: obj$so8 (
# CHECK-NEXT: Symbol: vt$ap16 (
# CHECK-NEXT: }
# CHECK-NOT:  Name:
# CHECK:      BaseReloc [
# CHECK-NEXT: ]
## Each word holds the lookup entry of the name it is bound to.
# CHECK:      Contents of section .rdata:
# CHECK:       180001040 {{.*}}  ..........obj...
# CHECK-NEXT:  180001050 {{.*}}  obj$so8...vt$ap1
# CHECK:      Contents of section .data:
# CHECK-NEXT:  180002000 48100000 00000000 4e100000 00000000
# CHECK-NEXT:  180002010 58100000 00000000

## With no entry and no read-only slot there is no IAT directory; vt's symbol
## stays for the debug section that names it.
# DEBUG:     IATRVA: 0x0
# DEBUG:     Name: imp.dll
# DEBUG-NOT: Name: imp.dll

# BAD:     error: bad.obj: .data holds the address of obj plus 16 at offset 0x0, but imp.dll exports neither obj$so16 nor obj$ap16; compute the address at run time
# BAD:     error: bad.obj: .data holds the address of obj plus -8 at offset 0x8, but imp.dll exports neither obj$so-8 nor obj$ap-8; compute the address at run time
# BAD:     error: bad.obj: .data holds the address of obj plus 32 at offset 0x10, but imp.dll exports neither obj$so32 nor obj$ap32; compute the address at run time
# BAD:     error: bad.obj: .data holds the address of fn plus 4 at offset 0x18, but imp.dll exports neither fn$so4 nor fn$ap4; compute the address at run time

#--- imp.def
LIBRARY imp.dll
EXPORTS
  fn obj DATA obj$so8 DATA obj$so24 DATA obj$so-8 DATA vt DATA vt$ap16 DATA

#--- other.def
LIBRARY other.dll
EXPORTS
obj$so16 DATA

#--- data.s
.data
  .quad obj
  .quad obj+8
  .quad vt+16

#--- debug.s
.section .debug_info,"drD"
  .quad vt

#--- bad.s
.data
  .quad obj+16
  .quad obj-8
  .quad obj+32
  .quad fn+4

#--- member.s
.data
.globl __imp_obj$so32
__imp_obj$so32:
  .quad 0
