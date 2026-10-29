// Experimental RAM-only BL31 callback 2. No caller-supplied pointers.
// The sole write operation controls seven DMSS statistics-enable bits.
// x0 selector, w1 AMER cookie; return a sign-extended 32-bit result via stock SMC.
.section .text, "ax"
.global monitor_probe
monitor_probe:
    mov w2, #0x4552
    movk w2, #0x414d, lsl #16
    cmp w1, w2
    b.ne rejected
    cmp x0, #1
    b.eq version
    sub x2, x0, #2
    cmp x2, #4
    b.hi other_selector
    adr x4, fixed_words
    ldr x4, [x4, x2, lsl #3]
    b read_word
other_selector:
    cmp x0, #0x100
    b.hs statistics_control
    cmp x0, #0x80
    b.hs protection_read
    sub x0, x0, #16
    cmp x0, #55
    b.hi rejected
    lsr w2, w0, #3
    and w3, w0, #7
    adr x4, offsets
    ldr w3, [x4, w3, uxtw #2]
    mov x4, #0xfffc0000
    add x4, x4, x2, lsl #11
    add x4, x4, x3
    b read_word
read_word:
    ldr w0, [x4]
    ret
version:
    mov w0, #0x0003
    movk w0, #0x414d, lsl #16
    ret
statistics_control:
    sub x0, x0, #0x100
    cmp x0, #0x7f
    b.hi rejected
    mov x4, #0x6260
    movk x4, #0xfffc, lsl #16
    ldr w3, [x4]
    and w2, w3, #0xffffff80
    orr w2, w2, w0
    str w2, [x4]
    dsb sy
    mov w0, w3
    ret
protection_read:
    sub x0, x0, #0x80
    cmp x0, #63
    b.hi rejected
    // ASI6 has 16 regions, verified from RTL_INF3. Read-only policy snapshot.
    mov x4, #0x3500
    movk x4, #0xfffc, lsl #16
    add x4, x4, x0, lsl #2
    b read_word
rejected:
    mov w0, #-1
    ret
.balign 4
offsets:
    .word 0x00c, 0x200, 0x208, 0x210, 0x214, 0x218, 0x21c, 0x424
.balign 8
fixed_words:
    .quad 0xfffc6000, 0xfffc6004, 0xe8a09c0c, 0xe8a09068, 0xfffc6260
