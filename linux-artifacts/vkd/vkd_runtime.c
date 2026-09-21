#define _POSIX_C_SOURCE 200809L
#include "vkd_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <unistd.h>

int vkd_runtime_init(void) {
    if (getpid() == 1 &&
        (mount("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, NULL) ||
         mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) ||
         mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL))) {
        return -1;
    }
    return setenv("VK_DRIVER_FILES", "/usr/share/vulkan/icd.d/lvp_icd.json", 0) ||
                   setenv("LP_NUM_THREADS", "1", 1) ||
                   setenv("MESA_SHADER_CACHE_DISABLE", "true", 1)
               ? -1
               : 0;
}

int vkd_runtime_exit(int status) {
    fflush(stdout);
    fflush(stderr);
    if (getpid() == 1) {
        for (;;) {
            pause();
        }
    }
    return status;
}
