#pragma once

#include "midix/abi/types.h"

namespace midix::format {

// Standard Ed25519 (SHA-512), not Monocypher's default Blake2b signature scheme.
// The caller selects the public key from its configured trust store.
bool verify_ed25519_signature(const mx_u8_t signature[64], const mx_u8_t public_key[32],
                              const mx_u8_t* message, mx_size_t message_size);

} // namespace midix::format
