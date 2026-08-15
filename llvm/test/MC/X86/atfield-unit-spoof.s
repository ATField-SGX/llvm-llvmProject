# RUN: not llvm-mc -triple=x86_64-unknown-linux-gnu -filetype=obj %s -o /dev/null 2>&1 | FileCheck %s
.text
.atfield_function_begin 0, 0
.atfield_unit_begin 1, 0, 2, 0
# A spoofed close/reopen sequence cannot make the real outer close valid.
.atfield_unit_end 0
.atfield_unit_begin 1, 1, 2, 0
.atfield_unit_end 0
.atfield_function_end

# CHECK: mismatched ATField source assembly unit ordinal
