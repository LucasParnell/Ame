# Experimental native target, matched to the ANE-LX1 stock kernel backup.
# Android header kernel_addr=0x00480000; ARM64 Image text_offset=0x80000.
set(ANKO_LOAD_BASE 0x00480000)
set(ANKO_IMAGE_TEXT_OFFSET 0x00080000)
set(ANKO_CONSOLE_KIND PL011)
set(ANKO_FB_WIDTH 1080)
set(ANKO_FB_HEIGHT 2280)
# Kirin 659 uses Cortex-A53 cores. Tune scheduling without enabling ARMv8.1
# features (notably LSE) absent from the primary device.
set(AME_CPU_TUNE cortex-a53)
