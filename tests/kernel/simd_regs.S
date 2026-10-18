.section .text, "ax"
.balign 4
.global test_simd_seed
.type test_simd_seed, %function
test_simd_seed:
    // Each register gets a different byte, including both 64-bit halves.
    .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    dup v\n\().16b, w0
    add w0, w0, #1
    .endr
    msr fpcr, x1
    msr fpsr, x2
    ret
.size test_simd_seed, . - test_simd_seed

.global test_simd_capture
.type test_simd_capture, %function
test_simd_capture:
    // Inspect via scalar lane extraction, independently of the production
    // Q-register load/store pairs and their memory offsets.
    .set offset, 0
    .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    umov x1, v\n\().d[0]
    umov x2, v\n\().d[1]
    stp x1, x2, [x0, #offset]
    .set offset, offset + 16
    .endr
    mrs x1, fpsr
    str w1, [x0, #512]
    mrs x1, fpcr
    str w1, [x0, #516]
    ret
.size test_simd_capture, . - test_simd_capture
