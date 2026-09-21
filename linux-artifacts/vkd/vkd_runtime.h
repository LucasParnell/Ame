#pragma once
/* Process setup shared by the guest service and its Linux acceptance probe. */
int vkd_runtime_init(void);
int vkd_runtime_exit(int status);
