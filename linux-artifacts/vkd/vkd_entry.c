#include "vkd_runtime.h"
#include <stdio.h>

int dango_vkd_run(void);

int main(void) {
    if (vkd_runtime_init()) {
        puts("dango-vkd: runtime setup FAILED");
        return vkd_runtime_exit(1);
    }
    return vkd_runtime_exit(dango_vkd_run() ? 0 : 1);
}
