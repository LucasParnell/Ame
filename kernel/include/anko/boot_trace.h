#pragma once
#ifdef ANKO_BOARD_HUAWEI_ANNE
#include "boot_diagnostics.h"
#define ANKO_BOOT_CHECKPOINT(stage) ::anne::boot_checkpoint(::anne::BootStage::stage)
#else
#define ANKO_BOOT_CHECKPOINT(stage) ((void)0)
#endif
