#pragma once
// anko/sdmmc.h — minimal T210 SDMMC1 (microSD) driver interface.
//
// After U-Boot transfers control, SDMMC1 is already clocked and powered.
// sdmmc_init() does a CMD/DAT soft-reset then re-enumerates from CMD0.
// Only PIO mode; adequate for a small log write (~2 MB/s at 25 MHz 4-bit).

#include "anko/types.h"

namespace drivers::sdmmc {

// Reset CMD/DAT paths and re-enumerate the SD card.  Returns true on success.
// Safe to call once after U-Boot handoff.
bool init();

// Read one 512-byte sector at LBA lba into buf (must be >= 512 bytes).
bool read_sector(u32 lba, void* buf);

// Write one 512-byte sector at LBA lba from buf.
bool write_sector(u32 lba, const void* buf);

} // namespace drivers::sdmmc
