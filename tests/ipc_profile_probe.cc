// Host probe for the on-disk IPC profile: strict decode, clamping, roundtrip.
#include <cassert>
#include <cstdio>
#include <cstring>
#include "mochi/ipc_profile.h"

int main() {
    using mochi::IpcProfile;
    const uint32_t db = mochi::profile_name_hash("drv.blk");
    const uint32_t msh = mochi::profile_name_hash("msh");

    IpcProfile p;
    assert(p.set(db, 1024) && p.set(msh, 64));
    assert(p.lookup(db, 7) == 1024 && p.lookup(msh, 7) == 64);
    assert(p.lookup(1234, 7) == 7);
    assert(p.set(db, 99999) && p.lookup(db, 0) == mochi::kIpcProfileMaxBudget);

    uint8_t file[8 + IpcProfile::kMax * 16]{};
    const uint32_t size = p.encode(file, sizeof(file));
    assert(size == 8 + 2 * 16);

    IpcProfile q;
    assert(q.decode(file, size) && q.count == 2 && q.lookup(db, 0) == mochi::kIpcProfileMaxBudget);

    // Every corruption is rejected wholesale.
    uint8_t bad[sizeof(file)];
    std::memcpy(bad, file, size); bad[0] = 'X';
    assert(!q.decode(bad, size));
    std::memcpy(bad, file, size); bad[5] = 9;
    assert(!q.decode(bad, size));
    std::memcpy(bad, file, size); bad[9] ^= 0x40;          // entry checksum
    assert(!q.decode(bad, size));
    assert(!q.decode(file, size - 1));                      // truncated
    std::memcpy(bad, file, size); bad[6] = 0xff; bad[7] = 0xff;
    assert(!q.decode(bad, size));                           // overlong count
    assert(!q.decode(nullptr, 0));

    // A rejected decode must not wipe the last good profile.
    assert(q.count == 2);

    std::puts("profile: roundtrip, clamp, strict rejection, last-good preserved");
    return 0;
}
