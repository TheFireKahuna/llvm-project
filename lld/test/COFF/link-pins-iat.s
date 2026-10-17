## The address table holds only func3, which a call reaches through its thunk,
## and ends at 0x10; func1 and func2 are kept in their slots. s32 and s16 fill
## the padding before vt1, s8 follows it, and vt2 takes its pin at 3832. The
## directory reaches vt2's end.
# CHECK:      140002000 R __imp_func3
# CHECK-NEXT: 140002010 r s32
# CHECK-NEXT: 140002030 r s16
