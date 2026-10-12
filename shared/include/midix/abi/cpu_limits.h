#pragma once
// Shared kernel/userland limit; also safe to include from preprocessed assembly.
#ifdef ANKO_BOARD_HUAWEI_ANNE
#define MX_HOST_MAX_CPUS 8
#else
#define MX_HOST_MAX_CPUS 4
#endif
