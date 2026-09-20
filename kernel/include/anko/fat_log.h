#pragma once
// anko/fat_log.h — minimal FAT32 boot log writer interface.
//
// Accumulates all kernel output in a 64 KB DRAM buffer then writes it to
// /ame/log/boot.log on the microSD FAT32 partition when fat_log_flush() is
// called.  Requires the log file to be pre-created on the host PC:
//
//   mkdir -p <SD>/ame/log
//   dd if=/dev/zero of=<SD>/ame/log/boot.log bs=512 count=128
//
// fat_log_init() must be called after kernel_main's VBAR/console setup but
// before platform::probe(), so the probe output is captured.

#include "anko/types.h"

namespace fat_log {

// Initialise SDMMC1, locate /ame/log/boot.log on the FAT32 partition.
// Installs a secondary console sink so all subsequent putchar output goes
// to the in-memory buffer.  Returns false if SD or file is not found.
bool init();

// Write the accumulated buffer to the SD card log file.
// Safe to call multiple times; each call overwrites from the file start.
void flush();

// Append a NUL-terminated string directly (bypasses console sink, useful
// for messages emitted before the sink is installed).
void write(const char* s);

} // namespace fat_log
