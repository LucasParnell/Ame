#include "midix/format/bundle.h"
#include "midix/format/integrity.h"
#include "midix/format/json.h"
#include "midix/format/signature.h"
#include "libk/string.h"

namespace midix::format {
namespace {

constexpr mx_size_t kBundleChecksumOffset = __builtin_offsetof(MxBundleHeader, bundle_crc32c);
constexpr mx_size_t kMaxManifestSize = 64 * 1024;
constexpr mx_size_t kMaxObjectNameSize = 100;
constexpr mx_size_t kMaxTrustedKeys = 8;
constexpr mx_u64_t kManifestSchemaVersion = 2;

struct ManifestEnvelope {
    MxSignedManifestHeader header{};
    const mx_u8_t* body = nullptr;
    const mx_u8_t* signature = nullptr;
};

bool contains_range(mx_u64_t offset, mx_u64_t length, mx_u64_t available) {
    // Subtraction avoids wrapping offset + length for untrusted ranges.
    return offset <= available && length <= available - offset;
}

bool is_zeroed(const mx_u8_t* bytes, mx_size_t size) {
    for (mx_size_t index = 0; index < size; ++index) {
        if (bytes[index] != 0) {
            return false;
        }
    }
    return true;
}

bool hash_matches_json(json::Value value, const mx_u8_t expected[MX_SHA256_SIZE]) {
    char encoded[2 * MX_SHA256_SIZE + 1];
    if (!json::string(value, encoded, sizeof(encoded)) ||
        libk::strlen(encoded) != 2 * MX_SHA256_SIZE) {
        return false;
    }
    for (mx_size_t index = 0; index < MX_SHA256_SIZE; ++index) {
        const int high = json::hex(encoded[2 * index]);
        const int low = json::hex(encoded[2 * index + 1]);
        if (high < 0 || low < 0 || ((high << 4) | low) != expected[index]) {
            return false;
        }
    }
    return true;
}

const char* validate_bundle_header(const Bundle& bundle) {
    const auto& header = bundle.header;
    if (header.magic != MX_BUNDLE_MAGIC || header.version != MX_BUNDLE_VERSION ||
        header.header_size != sizeof(header)) {
        return "unsupported bundle format or version";
    }
    if (header.flags != 0 || !is_zeroed(header.reserved, sizeof(header.reserved))) {
        return "unsupported bundle flags";
    }
    if (header.hash_algorithm != MX_HASH_SHA256 ||
        header.signature_algorithm != MX_SIGNATURE_ED25519 ||
        header.checksum_algorithm != MX_CHECKSUM_CRC32C) {
        return "unsupported bundle integrity algorithms";
    }
    if (header.bundle_size != bundle.size || header.object_count == 0 ||
        header.object_count > kMaxObjects) {
        return "invalid bundle size or object capacity";
    }

    const mx_u64_t object_table_size = header.object_count * sizeof(MxBundleObject);
    if (header.object_table_offset != sizeof(header) ||
        !contains_range(header.object_table_offset, object_table_size, bundle.size)) {
        return "invalid object table range";
    }
    if (header.string_table_offset != header.object_table_offset + object_table_size ||
        !contains_range(header.string_table_offset, header.string_table_size, bundle.size)) {
        return "invalid string table range";
    }
    if (header.manifest_size < sizeof(MxSignedManifestHeader) ||
        !contains_range(header.manifest_offset, header.manifest_size, bundle.size)) {
        return "invalid manifest range";
    }

    const mx_u32_t checksum = integrity::crc32c_zero_field(
        bundle.data, bundle.size, kBundleChecksumOffset, sizeof(header.bundle_crc32c));
    if (checksum != header.bundle_crc32c) {
        return "bundle checksum mismatch";
    }
    return nullptr;
}

const char* read_manifest_envelope(const Bundle& bundle, ManifestEnvelope& envelope) {
    const mx_u8_t* manifest = bundle.data + bundle.header.manifest_offset;
    const mx_u64_t available = bundle.header.manifest_size;
    auto& header = envelope.header;
    libk::memcpy(&header, manifest, sizeof(header));

    if (header.magic != MX_SIGNED_MANIFEST_MAGIC || header.version != MX_SIGNED_MANIFEST_VERSION ||
        header.header_size != sizeof(header) || header.flags != 0 ||
        !is_zeroed(header.reserved, sizeof(header.reserved))) {
        return "unsupported signed manifest format";
    }
    if (header.hash_algorithm != MX_HASH_SHA256 ||
        header.signature_algorithm != MX_SIGNATURE_ED25519 ||
        header.public_key_id_size != MX_ED25519_PUBLIC_KEY_SIZE ||
        header.signature_size != MX_ED25519_SIGNATURE_SIZE) {
        return "unsupported manifest signature parameters";
    }
    if (header.body_offset != sizeof(header) || header.body_size == 0 ||
        header.body_size > kMaxManifestSize ||
        !contains_range(header.body_offset, header.body_size, available)) {
        return "invalid manifest body range";
    }
    if (header.signature_offset != header.body_offset + header.body_size ||
        !contains_range(header.signature_offset, header.signature_size, available) ||
        header.signature_offset + header.signature_size != available) {
        return "invalid manifest signature range";
    }

    envelope.body = manifest + header.body_offset;
    envelope.signature = manifest + header.signature_offset;
    return nullptr;
}

const mx_u8_t* find_trusted_key(const ManifestEnvelope& envelope,
                                const mx_u8_t (*trusted_keys)[MX_ED25519_PUBLIC_KEY_SIZE],
                                mx_size_t key_count) {
    for (mx_size_t index = 0; index < key_count; ++index) {
        mx_u8_t key_id[MX_SHA256_SIZE];
        integrity::sha256(trusted_keys[index], MX_ED25519_PUBLIC_KEY_SIZE, key_id);
        if (integrity::bytes_equal(key_id, envelope.header.public_key_id, sizeof(key_id))) {
            return trusted_keys[index];
        }
    }
    return nullptr;
}

const char* authenticate_manifest(const ManifestEnvelope& envelope,
                                  const mx_u8_t (*trusted_keys)[MX_ED25519_PUBLIC_KEY_SIZE],
                                  mx_size_t key_count) {
    mx_u8_t digest[MX_SHA256_SIZE];
    integrity::sha256(envelope.body, envelope.header.body_size, digest);
    if (!integrity::bytes_equal(digest, envelope.header.body_sha256, sizeof(digest))) {
        return "manifest digest mismatch";
    }

    const mx_u8_t* public_key = find_trusted_key(envelope, trusted_keys, key_count);
    if (!public_key) {
        return "manifest key is not trusted";
    }
    if (!verify_ed25519_signature(envelope.signature, public_key, envelope.body,
                                  envelope.header.body_size)) {
        return "manifest signature rejected";
    }
    return nullptr;
}

const char* read_signed_catalog(const Bundle& bundle, json::Value& catalog) {
    // The terminating NUL belongs to the signed body and the bootstrap page.
    // Only the JSON parser receives a length that excludes the terminator.
    if (bundle.manifest[bundle.manifest_size - 1] != '\0') {
        return "manifest missing terminator";
    }
    const auto document = json::parse(bundle.manifest, bundle.manifest_size - 1);
    mx_u64_t schema = 0;
    if (!document || !json::number(json::member(document, "schema"), schema) ||
        schema != kManifestSchemaVersion) {
        return "unsupported manifest schema";
    }

    catalog = json::member(document, "catalog");
    if (!catalog || *catalog.begin != '[') {
        return "missing signed catalog";
    }
    return nullptr;
}

bool has_duplicate_name(const Bundle& bundle, mx_u32_t index, const char* name) {
    for (mx_u32_t previous = 0; previous < index; ++previous) {
        MxBundleObject ignored{};
        const char* previous_name = nullptr;
        if (!object(bundle, previous, ignored, previous_name) ||
            libk::strcmp(name, previous_name) == 0) {
            return true;
        }
    }
    return false;
}

bool object_digest_matches(const Bundle& bundle, const MxBundleObject& entry) {
    const mx_u8_t* payload = bundle.data + entry.offset;
    mx_u8_t digest[MX_SHA256_SIZE];
    integrity::sha256(payload, entry.size, digest);
    return integrity::bytes_equal(digest, entry.sha256, sizeof(digest)) &&
           integrity::crc32c(payload, entry.size) == entry.crc32c;
}

bool catalog_entry_matches(json::Value catalog, mx_u32_t index, const MxBundleObject& entry,
                           const char* name) {
    const auto record = json::element(catalog, index);
    mx_u64_t kind = 0;
    mx_u64_t size = 0;
    if (!record || !json::equals(json::member(record, "name"), name) ||
        !json::number(json::member(record, "kind"), kind) ||
        !json::number(json::member(record, "size"), size)) {
        return false;
    }

    const bool supported_kind = kind == MX_BUNDLE_OBJECT_EXEC || kind == MX_BUNDLE_OBJECT_LIBRARY ||
                                kind == MX_BUNDLE_OBJECT_RESOURCE;
    return supported_kind && kind == entry.kind && size == entry.size &&
           hash_matches_json(json::member(record, "sha256"), entry.sha256);
}

const char* validate_object_catalog(const Bundle& bundle, json::Value catalog) {
    const auto& header = bundle.header;
    mx_u64_t previous_end = header.string_table_offset + header.string_table_size;

    for (mx_u32_t index = 0; index < header.object_count; ++index) {
        MxBundleObject entry{};
        const char* name = nullptr;
        if (!object(bundle, index, entry, name)) {
            return "invalid bundle object range or name";
        }
        if (entry.flags != 0 || entry.reserved != 0 || entry.alignment == 0 ||
            entry.alignment > 4096 || (entry.alignment & (entry.alignment - 1)) != 0 ||
            entry.offset % entry.alignment != 0 || entry.offset < previous_end) {
            return "invalid bundle object metadata";
        }
        previous_end = entry.offset + entry.size;

        if (has_duplicate_name(bundle, index, name)) {
            return "duplicate object";
        }
        if (!object_digest_matches(bundle, entry)) {
            return "object digest mismatch";
        }

        // The envelope cannot include its own hash in the signed body. Its
        // location and name are fixed; every other object requires a catalog entry.
        if (index == 0) {
            if (entry.kind != MX_BUNDLE_OBJECT_MANIFEST ||
                libk::strcmp(name, "manifest.json") != 0 ||
                entry.offset != header.manifest_offset || entry.size != header.manifest_size) {
                return "manifest selection mismatch";
            }
        } else if (!catalog_entry_matches(catalog, index - 1, entry, name)) {
            return "signed catalog mismatch";
        }
    }

    if (json::element(catalog, header.object_count - 1)) {
        return "extra catalog entries";
    }
    return nullptr;
}

} // namespace

bool object(const Bundle& bundle, mx_u32_t index, MxBundleObject& entry, const char*& name) {
    entry = {};
    name = nullptr;
    const auto& header = bundle.header;
    if (!bundle.data || header.bundle_size > bundle.size || header.object_count > kMaxObjects ||
        index >= header.object_count) {
        return false;
    }
    if (!contains_range(header.object_table_offset, header.object_count * sizeof(entry),
                        bundle.size) ||
        !contains_range(header.string_table_offset, header.string_table_size, bundle.size)) {
        return false;
    }

    const mx_u64_t offset = header.object_table_offset + index * sizeof(entry);
    libk::memcpy(&entry, bundle.data + offset, sizeof(entry));
    if (entry.name_offset >= header.string_table_size ||
        !contains_range(entry.offset, entry.size, header.bundle_size)) {
        return false;
    }

    const char* candidate =
        reinterpret_cast<const char*>(bundle.data + header.string_table_offset + entry.name_offset);
    const mx_u64_t available = header.string_table_size - entry.name_offset;
    for (mx_u64_t character = 0; character < available && character < kMaxObjectNameSize;
         ++character) {
        const auto value = static_cast<unsigned char>(candidate[character]);
        if (value == 0) {
            if (character == 0) {
                return false;
            }
            name = candidate;
            return true;
        }
        if (value < 32 || value > 126) {
            return false;
        }
    }
    return false;
}

const char* verify_bundle(const mx_u8_t* data, mx_size_t size,
                          const mx_u8_t (*trusted_keys)[MX_ED25519_PUBLIC_KEY_SIZE],
                          mx_size_t key_count, Bundle& out) {
    out = {};
    if (!data || size < sizeof(MxBundleHeader)) {
        return "truncated bundle";
    }
    if (!trusted_keys || key_count == 0 || key_count > kMaxTrustedKeys) {
        return "invalid manifest trust store";
    }

    Bundle candidate{};
    candidate.data = data;
    candidate.size = size;
    libk::memcpy(&candidate.header, data, sizeof(candidate.header));
    if (const char* error = validate_bundle_header(candidate)) {
        return error;
    }

    ManifestEnvelope envelope;
    if (const char* error = read_manifest_envelope(candidate, envelope)) {
        return error;
    }
    if (const char* error = authenticate_manifest(envelope, trusted_keys, key_count)) {
        return error;
    }

    candidate.manifest = reinterpret_cast<const char*>(envelope.body);
    candidate.manifest_size = envelope.header.body_size;
    json::Value catalog;
    if (const char* error = read_signed_catalog(candidate, catalog)) {
        return error;
    }
    if (const char* error = validate_object_catalog(candidate, catalog)) {
        return error;
    }

    // Publish the view only after its signature and every object have passed.
    out = candidate;
    return nullptr;
}

} // namespace midix::format
