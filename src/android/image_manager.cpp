// PS5Droid - Android image handling (implementation).
#include "android/image_manager.h"

#include <cstdio>
#include <cstring>

#include "android/sha256.h"
#include "platform/platform.h"

namespace ps5droid {
namespace android {

ParseStatus parse_manifest(const std::string& text, ImageManifest* out) {
    if (out == nullptr)
        return ParseStatus::kBadValue;

    ImageManifest m;
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }

    bool any = false;
    std::string first_error;
    for (const std::string& raw : lines) {
        std::string line = raw;
        // strip \r and comments
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        size_t hash = line.find('#');
        if (hash != std::string::npos)
            line = line.substr(0, hash);
        // trim
        size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos)
            continue;
        size_t e = line.find_last_not_of(" \t");
        line = line.substr(b, e - b + 1);

        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            if (first_error.empty())
                first_error = "line without '=': " + line;
            continue;
        }
        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        // Trim both key and value.
        size_t kb = key.find_first_not_of(" \t");
        if (kb == std::string::npos) {
            if (first_error.empty())
                first_error = "empty key";
            continue;
        }
        size_t ke = key.find_last_not_of(" \t");
        key = key.substr(kb, ke - kb + 1);
        size_t vb = value.find_first_not_of(" \t");
        if (vb == std::string::npos) {
            if (first_error.empty())
                first_error = "empty value for " + key;
            continue;
        }
        value = value.substr(vb);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
            value.pop_back();
        any = true;

        if (key == "system_image")
            m.system_image = value;
        else if (key == "vendor_image")
            m.vendor_image = value;
        else if (key == "system_sha256")
            m.system_sha256 = value;
        else if (key == "vendor_sha256")
            m.vendor_sha256 = value;
        else if (key == "android_api_level")
            m.android_api_level = atoi(value.c_str());
        else if (key == "abi")
            m.abi = value;
        else if (key == "description")
            m.description = value;
        // unknown keys are ignored (forward compatibility)
    }

    if (first_error.empty() && !any)
        return ParseStatus::kEmpty;
    if (!first_error.empty())
        return ParseStatus::kSyntaxError;

    if (m.system_image.empty() || m.vendor_image.empty() || m.android_api_level <= 0 ||
        m.abi.empty()) {
        return ParseStatus::kMissingKeys;
    }
    *out = m;
    return ParseStatus::kOk;
}

ManifestCheck verify_manifest(const ImageManifest& manifest, const std::string& base_dir) {
    ManifestCheck r;

    auto full = [&](const std::string& name) {
        return base_dir.empty() ? name : base_dir + "/" + name;
    };

    r.system_exists = platform::file_exists(full(manifest.system_image));
    r.vendor_exists = platform::file_exists(full(manifest.vendor_image));

    r.system_digest_ok = true;
    if (r.system_exists && !manifest.system_sha256.empty()) {
        uint8_t digest[32];
        if (Sha256::hash_file(full(manifest.system_image), digest)) {
            r.system_digest_ok = Sha256::hex(digest) == manifest.system_sha256;
        } else {
            r.system_digest_ok = false;
        }
    }

    r.vendor_digest_ok = true;
    if (r.vendor_exists && !manifest.vendor_sha256.empty()) {
        uint8_t digest[32];
        if (Sha256::hash_file(full(manifest.vendor_image), digest)) {
            r.vendor_digest_ok = Sha256::hex(digest) == manifest.vendor_sha256;
        } else {
            r.vendor_digest_ok = false;
        }
    }

    if (!r.system_exists)
        r.failure_note = "system image missing: " + manifest.system_image;
    else if (!r.vendor_exists)
        r.failure_note = "vendor image missing: " + manifest.vendor_image;
    else if (!r.system_digest_ok)
        r.failure_note = "system image digest mismatch";
    else if (!r.vendor_digest_ok)
        r.failure_note = "vendor image digest mismatch";
    return r;
}

Ext4Probe probe_ext4(const std::string& image_path) {
    Ext4Probe r;

    // ext4 superblock: 1024 bytes into the image, 1024 bytes long. We read
    // the exact window instead of the whole image (they are multi-GB).
    FILE* f = fopen(image_path.c_str(), "rb");
    if (f == nullptr) {
        r.failure_note = "cannot open image";
        return r;
    }
    uint8_t sb[1024];
    if (fseek(f, 1024, SEEK_SET) != 0 || fread(sb, 1, sizeof(sb), f) != sizeof(sb)) {
        fclose(f);
        r.failure_note = "image too small or unreadable";
        return r;
    }
    fclose(f);

    uint16_t magic = static_cast<uint16_t>(sb[0x38]) | (static_cast<uint16_t>(sb[0x39]) << 8);
    if (magic != 0xEF53) {
        r.failure_note = "ext4 magic not found";
        return r;
    }

    r.is_ext4 = true;
    auto rd32 = [&](int off) {
        return static_cast<uint32_t>(sb[off]) | (static_cast<uint32_t>(sb[off + 1]) << 8) |
               (static_cast<uint32_t>(sb[off + 2]) << 16) |
               (static_cast<uint32_t>(sb[off + 3]) << 24);
    };
    r.blocks_count = rd32(0x04);
    uint32_t log_block_size = rd32(0x18);
    r.block_size = 1024u << log_block_size;

    char label[17];
    memcpy(label, sb + 0x78, 16);
    label[16] = '\0';
    r.volume_label = label;
    for (char& c : r.volume_label) {
        if (static_cast<unsigned char>(c) < 0x20)
            c = '\0';
    }
    return r;
}

ApkInspection inspect_apk(const std::string& apk_path) {
    ApkInspection res;
    FILE* fp = fopen(apk_path.c_str(), "rb");
    if (!fp) {
        res.failure_note = "cannot open APK file";
        return res;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    if (size < 30) {
        res.failure_note = "file too small to be a ZIP/APK";
        fclose(fp);
        return res;
    }

    fseek(fp, 0, SEEK_SET);
    uint8_t hdr[4];
    if (fread(hdr, 1, 4, fp) != 4 || hdr[0] != 'P' || hdr[1] != 'K' || hdr[2] != 0x03 ||
        hdr[3] != 0x04) {
        res.failure_note = "invalid ZIP/APK magic";
        fclose(fp);
        return res;
    }

    res.is_apk = true;
    res.file_count = 1;

    std::vector<uint8_t> buf(static_cast<size_t>(size));
    fseek(fp, 0, SEEK_SET);
    size_t read_bytes = fread(buf.data(), 1, buf.size(), fp);
    fclose(fp);

    const std::string content(reinterpret_cast<const char*>(buf.data()), read_bytes);
    if (content.find("AndroidManifest.xml") != std::string::npos) {
        res.has_manifest = true;
    }
    if (content.find("classes.dex") != std::string::npos) {
        res.has_classes_dex = true;
    }
    if (content.find("lib/x86_64") != std::string::npos ||
        content.find("lib/arm64-v8a") != std::string::npos) {
        res.has_native_libs = true;
    }

    // Honesty note: the package name lives in the binary AXML form of
    // AndroidManifest.xml (a compiled XML format). String-pool parsing is
    // NOT implemented in this project, so no package name is reported. A
    // placeholder would violate the repository's honesty policy
    // (docs/development.md, section 6).
    if (res.has_manifest) {
        res.package_note =
            "binary AXML string-pool parsing not implemented; package name not reported";
    }
    return res;
}

} // namespace android
} // namespace ps5droid
