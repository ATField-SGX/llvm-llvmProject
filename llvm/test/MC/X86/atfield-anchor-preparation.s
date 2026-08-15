# RUN: llvm-mc -triple=x86_64-unknown-linux-gnu -filetype=obj -o %t.o %s
# RUN: llvm-readelf -S %t.o | FileCheck %s --check-prefix=SECTIONS
# RUN: llvm-readelf -s %t.o | FileCheck %s --check-prefix=SYMBOLS
# RUN: llvm-objdump -s -j .text %t.o | FileCheck %s --check-prefix=TEXT
# RUN: llvm-objdump -s -j .note.atfield.anchors %t.o | FileCheck %s --check-prefix=NOTE

# The first unit is a complete fill fragment.  The second unit contains the
# natural C3.  The assembler must place the seven-byte NOP wrapper only at the
# first unit boundary and must preserve the fill and RET bytes.
.text
.atfield_function_begin 1, 0
.atfield_unit_begin 0, 0, 1, 0
.fill 2048, 1, 0x90
.atfield_unit_begin 0, 1, 1, 0
.fill 2048, 1, 0x90
ret
.atfield_function_end

# SECTIONS: .note.atfield.anchors
# SYMBOLS-COUNT-2: __atfield_anchor_p1_f0_a
# TEXT: 0f1f80c3 00000090
# TEXT: c3
# NOTE: 41464e32 01005000
