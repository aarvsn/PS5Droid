// PS5Droid - Android image and APK handling (read-only).
//
// PS5Droid cannot mount ext4 images on Orbis (no loop devices, no FUSE,
// no ext4 driver - see docs/platform-constraints.md). What we CAN do
// honestly today:
//   * parse image manifests (metadata about system/vendor images),
//   * validate manifest contents against the filesystem,
//   * verify SHA-256 digests (Waydroid's OTA model hashes archives the
//     same way; see tools/helpers/images.py in the waydroid repo),
//   * probe ext4 superblocks directly from the image bytes without
//     mounting,
//   * parse Android binary XML (AXML / AndroidManifest.xml) and APK archives,
//   * parse Android system properties (build.prop).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
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
struct Ext4Probe {
    bool is_ext4 = false;
    uint32_t blocks_count = 0;
    uint32_t block_size = 0;
    std::string volume_label;
    std::string failure_note;
};

Ext4Probe probe_ext4(const std::string& image_path);

// Android Binary XML (AXML) parser result.
struct BinaryAxmlResult {
    bool ok = false;
    std::string package_name;
    int min_sdk_version = 0;
    int target_sdk_version = 0;
    int version_code = 0;
    std::string version_name;
    size_t activity_count = 0;
    std::string failure_note;
};

// Parses binary AXML (AndroidManifest.xml).
BinaryAxmlResult parse_binary_axml(const uint8_t* data, size_t size);

// Milestone 6: APK archive and AndroidManifest structure inspection.
struct ApkInspection {
    bool is_apk = false;
    bool has_manifest = false;
    bool has_classes_dex = false;
    bool has_native_libs = false;
    std::vector<std::string> native_abis;
    std::string package_name;
    int min_sdk_version = 0;
    int target_sdk_version = 0;
    int version_code = 0;
    std::string version_name;
    size_t activity_count = 0;
    std::string package_note;
    size_t file_count = 0;
    std::string failure_note;
};

ApkInspection inspect_apk(const std::string& apk_path);

// Android system properties (build.prop) parser.
struct BuildProp {
    std::string release_version;
    int sdk_version = 0;
    std::string model;
    std::string brand;
    std::string build_id;
    std::vector<std::pair<std::string, std::string>> all_properties;
};

BuildProp parse_build_prop(const std::string& text);

} // namespace android
} // namespace ps5droid
