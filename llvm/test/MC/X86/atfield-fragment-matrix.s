# RUN: llvm-mc -triple=x86_64-unknown-linux-gnu -filetype=obj -o %t.o %s
# RUN: llvm-readelf -s %t.o | FileCheck %s --check-prefix=SYMBOLS
# RUN: llvm-objdump -s -j .text %t.o | FileCheck %s --check-prefix=TEXT
# RUN: llvm-objdump -s -j .note.atfield.anchors %t.o | FileCheck %s --check-prefix=NOTE

.text
.atfield_function_begin 2, 0
# Align, fill, and org fragments are indivisible unit 0.
.atfield_unit_begin 0, 0, 1, 0
.p2align 4, 0x90
.fill 16, 1, 0x90
.org 0x100, 0x90
# Nops and a relaxable branch are indivisible bundled unit 1.
.atfield_unit_begin 0, 1, 1, 1
.nops 16
jmp .Ltarget
# Inline assembly is one complete source-assembly unit 2.
.atfield_unit_begin 1, 2, 2, 0
.byte 0x90
.byte 0x90
.atfield_unit_end 2
# LEB and a long gap exercise variable and long fragments.
.atfield_unit_begin 0, 3, 1, 0
.uleb128 127
.atfield_unit_begin 0, 4, 1, 0
.fill 3000, 1, 0x90
.atfield_unit_begin 0, 5, 1, 0
.fill 2000, 1, 0x90
.atfield_unit_begin 0, 6, 1, 0
.Ltarget:
ret
.fill 4, 1, 0x90
.atfield_function_end

# SYMBOLS: __atfield_function_begin_p2_f0
# SYMBOLS: __atfield_function_end_p2_f0
# SYMBOLS: __atfield_anchor_p2_f0_a
# TEXT: 0f1f80c3 00000090
# TEXT: c3
# NOTE: 41464e32 01005000
