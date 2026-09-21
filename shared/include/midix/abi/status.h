#pragma once
// Midix syscall and protocol status codes. Negative values match the kernel ABI.

#include "midix/abi/types.h"

#if defined(__cplusplus)
enum : mx_status_t {
#else
enum {
#endif
    MX_OK                    = 0,
    MX_ERR_INVALID           = -1,
    MX_ERR_PERM              = -2,
    MX_ERR_NOMEM             = -3,
    MX_ERR_NOT_FOUND         = -4,
    MX_ERR_BAD_TYPE          = -5,
    MX_ERR_OVERFLOW          = -6,
    MX_ERR_DEAD              = -7,
    MX_ERR_BUSY              = -8,
    MX_ERR_FAULT             = -9,
    MX_ERR_NOT_IMPLEMENTED   = -10,
    MX_ERR_SERVICE_UNAVAILABLE = -11,
    MX_ERR_BAD_PROTOCOL      = -12,
    MX_ERR_WOULD_BLOCK       = -13,
    MX_ERR_BAD_IMAGE         = -14,
    MX_ERR_BAD_SIGNATURE     = -15,
    MX_ERR_BAD_CHECKSUM      = -16,
    MX_ERR_POLICY            = -17,
};

#if defined(__cplusplus)
static inline bool mx_ok(mx_status_t status) {
    return status >= 0;
}

static inline bool mx_err(mx_status_t status) {
    return status < 0;
}
#else
#define mx_ok(status) ((status) >= 0)
#define mx_err(status) ((status) < 0)
#endif
