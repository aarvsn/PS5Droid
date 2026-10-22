// PS5Droid - Android image handling (read-only, no mounting).
//
// PS5Droid cannot mount ext4 images on Orbis (no loop devices, no FUSE,
// no ext4 driver - see docs/platform-constraints.md). What we CAN do
// honestly today:
//   * parse image manifests (metadata about system/vendor images),
//   * validate manifest contents against the filesystem,
//   * verify SHA-256 digests (Waydroid's OTA model hashes archives the
//     same way; see tools/helpers/images.py in the waydroid repo),
//   * probe ext4 superblocks directly from the image bytes without
//     mounting.
//
// Mounting, extracting and running anything from these images is NOT
// implemented and is reported as blocked by the runtime manager.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ps5droid {
namespace android {

// Manifest format: "key = value" lines, '#' comments, blank lines ignored.
// Required keys: system_image, vendor_image, android_api_level, abi.
// Optional keys: system_sha256, vendor_sha256, description.
struct ImageManifest {
    std::string system_image;
    std::string vendor_image;
    std::string system_sha256;
    std::string vendor_sha256;
    int android_api_level = 0;
    std::string abi; // e.g. "x86_64"
    std::string description;
};

enum class ParseStatus { kOk, kEmpty, kSyntaxError, kMissingKeys, kBadValue };

ParseStatus parse_manifest(const std::string& text, ImageManifest* out);

// Verification results for a manifest relative to the current filesystem.
struct ManifestCheck {
    bool system_exists = false;
    bool vendor_exists = false;
    bool system_digest_ok = false; // true when digest matches or unset
    bool vendor_digest_ok = false;
    std::string failure_note; // set when any check fails
};

ManifestCheck verify_manifest(const ImageManifest& manifest, const std::string& base_dir);

// ext4 superblock probe (read-only, works without mounting).
// Layout: superblock starts at byte 1024 of the image.
//   +0x00 s_inodes_count      u32
//   +0x04 s_blocks_count_lo   u32
//   +0x18 s_log_block_size    u32   (block size = 1024 << value)
//   +0x38 s_magic             u16   (0xEF53)
//   +0x68 s_uuid              16 bytes
//   +0x78 s_volume_name       16 bytes
struct Ext4Probe {
    bool is_ext4 = false;
    uint32_t blocks_count = 0;
    uint32_t block_size = 0;
    std::string volume_label;
    std::string failure_note;
};

Ext4Probe probe_ext4(const std::string& image_path);

// Milestone 6: APK archive and AndroidManifest structure inspection.
struct ApkInspection {
    bool is_apk = false;
    bool has_manifest = false;
    bool has_classes_dex = false;
    bool has_native_libs = false;
    // Always empty in this release: binary AXML (AndroidManifest.xml) is
    // a compiled XML format whose string-pool parsing is NOT implemented
    // here, and the honesty policy forbids placeholder values. package_note
    // records that limitation explicitly.
    std::string package_name;
    std::string package_note;
    size_t file_count = 0;
    std::string failure_note;
};

ApkInspection inspect_apk(const std::string& apk_path);

} // namespace android
} // namespace ps5droid
