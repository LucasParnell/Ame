#include "midix/format/signature.h"
#include "monocypher-ed25519.h"

namespace midix::format {

bool verify_ed25519_signature(const mx_u8_t signature[64], const mx_u8_t public_key[32],
                              const mx_u8_t* message, mx_size_t message_size) {
    if (!signature || !public_key || (!message && message_size != 0)) {
        return false;
    }
    return crypto_ed25519_check(signature, public_key, message, message_size) == 0;
}

} // namespace midix::format
