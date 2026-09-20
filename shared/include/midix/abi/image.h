#pragma once
// Midix native image and bundle format ABI.
//
// MXI1 is the runtime executable/library image format.
// MXB1 is the signed bundle container for boot services, applications,
// libraries, and resources.

#include "midix/abi/types.h"

constexpr mx_u32_t MX_IMAGE_MAGIC = 0x3149584du;    // 'MXI1'
constexpr mx_u16_t MX_IMAGE_VERSION = 1;
constexpr mx_u32_t MX_IMAGE_ABI_VERSION = 2;

constexpr mx_u32_t MX_BUNDLE_MAGIC = 0x3142584du;   // 'MXB1'
constexpr mx_u16_t MX_BUNDLE_VERSION = 2;

constexpr mx_u32_t MX_SIGNED_MANIFEST_MAGIC = 0x314d584du; // 'MXM1'
constexpr mx_u16_t MX_SIGNED_MANIFEST_VERSION = 2;

constexpr mx_u32_t MX_HASH_NONE = 0;
constexpr mx_u32_t MX_HASH_SHA256 = 1;
constexpr mx_u32_t MX_CHECKSUM_CRC32C = 1;
constexpr mx_u32_t MX_SIGNATURE_NONE = 0;
constexpr mx_u32_t MX_SIGNATURE_ED25519 = 1;

constexpr mx_u32_t MX_SHA256_SIZE = 32;
constexpr mx_u32_t MX_ED25519_PUBLIC_KEY_SIZE = 32;
constexpr mx_u32_t MX_ED25519_SIGNATURE_SIZE = 64;

enum MxImageKind : mx_u32_t {
    MX_IMAGE_KIND_EXEC = 1,
    MX_IMAGE_KIND_LIBRARY = 2,
};

enum MxImageArch : mx_u32_t {
    MX_IMAGE_ARCH_AARCH64 = 1,
};

enum MxImageFlags : mx_u32_t {
    MX_IMAGE_FLAG_NONE = 0,
    MX_IMAGE_FLAG_STATIC = 1u << 0,
};

enum MxImageSegmentFlags : mx_u32_t {
    MX_IMAGE_SEG_ZERO_FILL = 1u << 0,
    MX_IMAGE_SEG_SHARED_TEXT = 1u << 1,
    MX_IMAGE_SEG_SNAPSHOT_STATE = 1u << 2,
    MX_IMAGE_SEG_NO_SNAPSHOT = 1u << 3,
};

enum MxImageRequirementFlags : mx_u16_t {
    MX_IMAGE_REQ_OPTIONAL = 1u << 0,
};

enum MxBundleObjectKind : mx_u32_t {
    MX_BUNDLE_OBJECT_EXEC = 1,
    MX_BUNDLE_OBJECT_LIBRARY = 2,
    MX_BUNDLE_OBJECT_RESOURCE = 3,
    MX_BUNDLE_OBJECT_MANIFEST = 4,
};

enum MxPolicyRestart : mx_u32_t {
    MX_POLICY_RESTART_NEVER = 0,
    MX_POLICY_RESTART_ALWAYS = 1,
};

enum MxPolicyPrincipal : mx_u32_t {
    MX_POLICY_PRINCIPAL_KERNEL = 0,
    MX_POLICY_PRINCIPAL_NATIVE_TRUSTED = 1,
    MX_POLICY_PRINCIPAL_NATIVE_SANDBOXED = 2,
    MX_POLICY_PRINCIPAL_VM = 3,
};

struct MxBundleHeader {
    mx_u32_t magic;
    mx_u16_t version;
    mx_u16_t header_size;
    mx_u32_t flags;
    mx_u32_t object_count;
    mx_u64_t object_table_offset;
    mx_u64_t manifest_offset;
    mx_u64_t manifest_size;
    mx_u64_t string_table_offset;
    mx_u64_t string_table_size;
    mx_u64_t bundle_size;
    mx_u32_t bundle_crc32c;
    mx_u32_t hash_algorithm;
    mx_u32_t signature_algorithm;
    mx_u32_t checksum_algorithm;
    mx_u8_t  reserved[48];
};
static_assert(sizeof(MxBundleHeader) == 128, "MXB1 header must stay stable");

struct MxBundleObject {
    mx_u32_t name_offset;
    mx_u32_t kind;
    mx_u64_t offset;
    mx_u64_t size;
    mx_u64_t alignment;
    mx_u8_t  sha256[MX_SHA256_SIZE];
    mx_u32_t crc32c;
    mx_u32_t flags;
    mx_u64_t reserved;
};
static_assert(sizeof(MxBundleObject) == 80, "MXB1 object record must stay stable");

struct MxSignedManifestHeader {
    mx_u32_t magic;
    mx_u16_t version;
    mx_u16_t header_size;
    mx_u32_t flags;
    mx_u32_t hash_algorithm;
    mx_u32_t signature_algorithm;
    mx_u32_t public_key_id_size;
    mx_u64_t body_offset;
    mx_u64_t body_size;
    mx_u64_t signature_offset;
    mx_u64_t signature_size;
    mx_u8_t  body_sha256[MX_SHA256_SIZE];
    mx_u8_t  public_key_id[MX_ED25519_PUBLIC_KEY_SIZE];
    mx_u8_t  reserved[8];
};
static_assert(sizeof(MxSignedManifestHeader) == 128, "MXM1 header must stay stable");

struct MxImageHeader {
    mx_u32_t magic;
    mx_u16_t version;
    mx_u16_t header_size;
    mx_u32_t kind;
    mx_u32_t arch;
    mx_u32_t abi_version;
    mx_u32_t flags;
    mx_u64_t entry_va;
    mx_u64_t stack_size;
    mx_u64_t image_size;
    mx_u64_t segment_table_offset;
    mx_u32_t segment_count;
    mx_u32_t requirement_count;
    mx_u64_t requirement_table_offset;
    mx_u32_t import_count;
    mx_u32_t export_count;
    mx_u64_t import_table_offset;
    mx_u64_t export_table_offset;
    mx_u64_t string_table_offset;
    mx_u64_t string_table_size;
    mx_u32_t image_crc32c;
    mx_u32_t checksum_algorithm;
    mx_u8_t  reserved[8];
};
static_assert(sizeof(MxImageHeader) == 128, "MXI1 header must stay stable");

struct MxImageSegment {
    mx_u64_t file_offset;
    mx_u64_t file_size;
    mx_u64_t memory_size;
    mx_u64_t virtual_address;
    mx_u64_t alignment;
    mx_rights_t rights;
    mx_u32_t flags;
    mx_u32_t reserved0;
    mx_u8_t  reserved[12];
};
static_assert(sizeof(MxImageSegment) == 64, "MXI1 segment must stay stable");

struct MxImageRequirement {
    mx_u32_t name_offset;
    mx_u16_t cap_type;
    mx_u16_t flags;
    mx_rights_t required_rights;
    mx_u32_t protocol;
    mx_u32_t protocol_version;
    mx_u64_t aux0;
    mx_u64_t aux1;
    mx_u8_t  reserved[24];
};
static_assert(sizeof(MxImageRequirement) == 64, "MXI1 requirement must stay stable");

struct MxPolicyRecord {
    mx_u32_t service_name_offset;
    mx_u32_t image_name_offset;
    mx_u32_t principal_class;
    mx_u32_t restart_policy;
    mx_u32_t scheduler_policy;
    mx_u32_t timer_precision_ns;
    mx_u64_t arena_soft_quota;
    mx_u64_t arena_hard_quota;
    mx_u32_t nameservice_scope_offset;
    mx_u32_t library_allowlist_offset;
    mx_u32_t cap_grant_offset;
    mx_u32_t cap_grant_count;
    mx_u8_t  reserved[72];
};
static_assert(sizeof(MxPolicyRecord) == 128, "MX policy record must stay stable");

struct MxPolicyCapGrant {
    mx_u32_t name_offset;
    mx_u16_t cap_type;
    mx_u16_t flags;
    mx_rights_t rights;
    mx_u32_t source_offset;
    mx_u64_t aux0;
    mx_u64_t aux1;
    mx_u8_t  reserved[32];
};
static_assert(sizeof(MxPolicyCapGrant) == 64, "MX policy cap grant must stay stable");
