// kernel/arch/arm64/user_stub.S
//
// Minimal EL0 bring-up program for V0:
//   - validates EL0 entry and syscall path
//   - prints first userspace console milestone markers

.section .rodata.userstub, "a"
msg_hello:
    .ascii "hello world\n"
msg_hello_end:
msg_console:
    .ascii "CONSOLE OK\n"
msg_console_end:
msg_boot_ok:
    .ascii "MIDIX V0 BOOT OK\n"
msg_boot_ok_end:

.section .text.userstub, "ax"
.global user_init_stub
.type user_init_stub, %function
user_init_stub:
    adr     x0, msg_hello
    mov     x1, #(msg_hello_end - msg_hello)
    mov     x8, #17         // SYS_DEBUG_PUTS
    svc     #0

    adr     x0, msg_console
    mov     x1, #(msg_console_end - msg_console)
    mov     x8, #17         // SYS_DEBUG_PUTS
    svc     #0

    adr     x0, msg_boot_ok
    mov     x1, #(msg_boot_ok_end - msg_boot_ok)
    mov     x8, #17         // SYS_DEBUG_PUTS
    svc     #0

    mov     x0, #0
    mov     x8, #1          // SYS_EXIT
    svc     #0
1:
    wfe
    b       1b
