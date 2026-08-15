# RUN: llvm-mc -triple=x86_64-unknown-linux-gnu -filetype=obj -o %t.o %s
# RUN: llvm-readelf -s %t.o | FileCheck %s --check-prefix=SYMBOLS
# RUN: llvm-objdump -s -j .note.atfield.anchors %t.o | FileCheck %s --check-prefix=NOTE
.text
.atfield_function_begin 0, 0
.atfield_unit_begin 0, 0, 1, 0
nop
.atfield_function_end

# SYMBOLS: __atfield_function_begin_p0_f0
# SYMBOLS: __atfield_function_end_p0_f0
# SYMBOLS-NOT: __atfield_anchor_p
# NOTE: 41464e32 01005000 02000000
