# boards/switch-t210/board.cmake — Nintendo Switch Tegra X1 T210 compile-time board constants.
set(ANKO_LOAD_BASE 0x80080000)
set(ANKO_IMAGE_TEXT_OFFSET 0x00080000)
set(ANKO_CONSOLE_KIND Tegra16550)
# Switch screen: 1280×720 XRGB8888, Hekate pre-initialises TegraDC0.
set(ANKO_FB_WIDTH 1280)
set(ANKO_FB_HEIGHT 720)
# T210 XUSB: Hekate pre-loads FALCON firmware; the driver must not issue HCRST.
set(ANKO_XHCI_NORESET 1)
