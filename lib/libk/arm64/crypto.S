.arch armv8-a+crypto+crc
.text
.balign 16

// x0 = state, x1 = input, x2 = block count, x3 = SHA-256 round constants.
// No overread, heap, privileged instructions or hidden ISA detection.
.global libk_arm64_sha256_blocks
.type libk_arm64_sha256_blocks, %function
libk_arm64_sha256_blocks:
    cbz x2, .Lsha_done
    ld1 {v0.4s, v1.4s}, [x0]

.Lsha_block:
    mov v6.16b, v0.16b
    mov v7.16b, v1.16b
    ld1 {v2.16b, v3.16b, v4.16b, v5.16b}, [x1], #64
    rev32 v2.16b, v2.16b
    rev32 v3.16b, v3.16b
    rev32 v4.16b, v4.16b
    rev32 v5.16b, v5.16b
    mov x4, x3
    mov x5, #4

.macro sha_quad w, a, b, c
    ld1 {v18.4s}, [x4], #16
    add v16.4s, \w\().4s, v18.4s
    mov v17.16b, v0.16b
    sha256h q0, q1, v16.4s
    sha256h2 q1, q17, v16.4s
    sha256su0 \w\().4s, \a\().4s
    sha256su1 \w\().4s, \b\().4s, \c\().4s
.endm

.Lsha_rounds:
    sha_quad v2, v3, v4, v5
    sha_quad v3, v4, v5, v2
    sha_quad v4, v5, v2, v3
    sha_quad v5, v2, v3, v4
    subs x5, x5, #1
    b.ne .Lsha_rounds

    add v0.4s, v0.4s, v6.4s
    add v1.4s, v1.4s, v7.4s
    subs x2, x2, #1
    b.ne .Lsha_block
    st1 {v0.4s, v1.4s}, [x0]
.Lsha_done:
    ret
.size libk_arm64_sha256_blocks, . - libk_arm64_sha256_blocks

// Incremental reflected Castagnoli state, without the final complement.
.balign 16
.global libk_arm64_crc32c
.type libk_arm64_crc32c, %function
libk_arm64_crc32c:
    cbz x2, .Lcrc_done
.Lcrc_align:
    tst x1, #7
    b.eq .Lcrc_words
    ldrb w3, [x1], #1
    crc32cb w0, w0, w3
    subs x2, x2, #1
    b.eq .Lcrc_done
    b .Lcrc_align

.Lcrc_words:
    cmp x2, #8
    b.lo .Lcrc_tail
    ldr x3, [x1], #8
    crc32cx w0, w0, x3
    sub x2, x2, #8
    b .Lcrc_words

.Lcrc_tail:
    cbz x2, .Lcrc_done
    ldrb w3, [x1], #1
    crc32cb w0, w0, w3
    sub x2, x2, #1
    b .Lcrc_tail
.Lcrc_done:
    ret
.size libk_arm64_crc32c, . - libk_arm64_crc32c
