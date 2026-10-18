#pragma once
#include "midix/abi/types.h"
#if defined(AME_FIDO2_ATTESTATION) && AME_FIDO2_ATTESTATION
#include "attestation_cert.h"
#endif
namespace mochi::fido2 {
// Ame's model AAGUID. Generated once with uuidgen; changing it changes the
// model identity seen by relying parties and any metadata registered for it.
inline constexpr mx_u8_t kAaguid[16] = {
    0xdc, 0x0c, 0xdc, 0x63, 0x81, 0x70, 0x4f, 0x9d,
    0x83, 0xfc, 0x07, 0x27, 0x0e, 0x41, 0x3b, 0xd9,
};
inline constexpr char kAaguidText[] = "dc0cdc63-8170-4f9d-83fc-07270e413bd9";
#if defined(AME_FIDO2_ATTESTATION) && AME_FIDO2_ATTESTATION
inline constexpr bool kCertificateAttestation = true;
#else
inline constexpr bool kCertificateAttestation = false;
#endif
} // namespace mochi::fido2
