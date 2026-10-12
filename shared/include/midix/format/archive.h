#pragma once
// Read-only lookup in a caller-bounded boot archive (MXB1 or legacy ustar).
// This is structural decoding, not authentication: boot callers must obtain the
// immutable archive through the kernel's verified boot capability.
#include "libk/bytes.h"

namespace midix::format {

bool archive_find(libk::const_bytes archive, const char* name, libk::const_bytes* out);

} // namespace midix::format
