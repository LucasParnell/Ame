#pragma once
/* Bounded triangle diagnostic. Native control and guest control use the same
 * records. Pixels live in one shared CPU-only arena, never in ring payloads. */
#define AME_GPU_DIAG_TRIANGLE 0x47504401U
#define AME_GPU_DIAG_REPLY 0x47504402U
#define AME_GPU_DIAG_PRESENT 0x47504403U
#define AME_GPU_DIAG_WIDTH 512U
#define AME_GPU_DIAG_HEIGHT 512U
#define AME_GPU_DIAG_BYTES (AME_GPU_DIAG_WIDTH * AME_GPU_DIAG_HEIGHT * 4U)
#define AME_GPU_DIAG_TARGETS 2U
#define AME_GPU_DIAG_SHARED_BYTES (AME_GPU_DIAG_BYTES * AME_GPU_DIAG_TARGETS)
#define AME_GPU_DIAG_DURATION_MS 5000U
#define AME_GPU_DIAG_HARDWARE 1U
#define AME_GPU_DIAG_SCANOUT 2U
// Request reserved field: zero retains the original blocking presentation.
#define AME_GPU_DIAG_STREAM 1U
#define AME_GPU_DIAG_RESTORE 2U
#define AME_GPU_DIAG_SLOT1 4U
struct ame_gpu_diag_request {
    unsigned long long id;
    unsigned int width, height, duration_ms, reserved;
};
struct ame_gpu_diag_reply {
    unsigned long long id;
    int status;
    unsigned int flags;
    unsigned long long render_ns;
    unsigned int checksum, gpu_id;
};
#if defined(__cplusplus)
static_assert(sizeof(ame_gpu_diag_request) == 24);
static_assert(sizeof(ame_gpu_diag_reply) == 32);
#endif
