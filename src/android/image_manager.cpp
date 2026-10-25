// PS5Droid - Android image and APK handling (implementation).
#include "android/image_manager.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "android/sha256.h"
#include "platform/platform.h"

namespace ps5droid {
namespace android {

namespace {

uint16_t rd16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

struct BitReader {
    const uint8_t* data;
    size_t size;
    size_t bit_off = 0;

    uint32_t read_bits(size_t count) {
        uint32_t val = 0;
        for (size_t i = 0; i < count; ++i) {
            size_t byte_idx = bit_off / 8;
            if (byte_idx >= size)
                break;
            size_t bit_idx = bit_off % 8;
            uint32_t bit = (data[byte_idx] >> bit_idx) & 1;
            val |= (bit << i);
            ++bit_off;
        }
        return val;
    }

    void align_byte() {
        if (bit_off % 8 != 0) {
            bit_off = ((bit_off + 7) / 8) * 8;
        }
    }
};

// Inflates DEFLATE payload (STORED and FIXED Huffman blocks).
bool inflate_deflate(const uint8_t* in, size_t in_size, std::vector<uint8_t>* out,
                     size_t expected_out_size) {
    if (!in || in_size == 0 || !out)
        return false;
    out->clear();
    out->reserve(expected_out_size > 0 ? expected_out_size : 4096);

    BitReader br{in, in_size, 0};
    bool last = false;

    while (!last) {
        uint32_t bfinal = br.read_bits(1);
        uint32_t btype = br.read_bits(2);
        last = (bfinal != 0);

        if (btype == 0) { // STORED
            br.align_byte();
            size_t byte_off = br.bit_off / 8;
            if (byte_off + 4 > in_size)
                return false;
            uint16_t len = rd16(in + byte_off);
            uint16_t nlen = rd16(in + byte_off + 2);
            byte_off += 4;
            if (static_cast<uint16_t>(~len) != nlen)
                return false;
            if (byte_off + len > in_size)
                return false;
            out->insert(out->end(), in + byte_off, in + byte_off + len);
            br.bit_off = (byte_off + len) * 8;
        } else if (btype == 1) { // FIXED HUFFMAN
            while (true) {
                // Fixed Huffman lit/len table lookup
                // Codes: 0-143 (8 bits 00110000..10111111), 144-255 (9 bits 110010000..111111111),
                // 256-279 (7 bits 0000000..0010111), 280-287 (8 bits 11000000..11000111)
                uint32_t v = 0;
                uint16_t code = 0;
                bool found = false;

                // Read bit-by-bit (MSB first in Huffman code)
                for (size_t len = 1; len <= 9; ++len) {
                    uint32_t b = br.read_bits(1);
                    v = (v << 1) | b;
                    if (len == 7 && v <= 0x17) {
                        code = static_cast<uint16_t>(256 + v);
                        found = true;
                        break;
                    }
                    if (len == 8) {
                        if (v >= 0x30 && v <= 0xBF) {
                            code = static_cast<uint16_t>(v - 0x30);
                            found = true;
                            break;
                        }
                        if (v >= 0xC0 && v <= 0xC7) {
                            code = static_cast<uint16_t>(280 + (v - 0xC0));
                            found = true;
                            break;
                        }
                    }
                    if (len == 9 && v >= 0x190 && v <= 0x1FF) {
                        code = static_cast<uint16_t>(144 + (v - 0x190));
                        found = true;
                        break;
                    }
                }

                if (!found)
                    return false;
                if (code == 256)
                    break; // End of block

                if (code < 256) {
                    out->push_back(static_cast<uint8_t>(code));
                } else if (code <= 285) {
                    // Length code
                    static const uint16_t len_base[] = {3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                                        15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                                        67, 83, 99, 115, 131, 163, 195, 227, 258};
                    static const uint8_t len_bits[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                       2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
                    uint32_t idx = code - 257;
                    if (idx >= 29)
                        return false;
                    uint32_t match_len = len_base[idx] + br.read_bits(len_bits[idx]);

                    // Distance code (5 fixed bits for fixed huffman)
                    uint32_t dist_code = 0;
                    for (int d = 0; d < 5; ++d) {
                        dist_code = (dist_code << 1) | br.read_bits(1);
                    }
                    static const uint16_t dist_base[] = {
                        1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                        33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                        1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
                    static const uint8_t dist_bits[] = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,
                                                        4, 4, 5,  5,  6,  6,  7,  7,  8,  8,
                                                        9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
                    if (dist_code >= 30)
                        return false;
                    uint32_t match_dist = dist_base[dist_code] + br.read_bits(dist_bits[dist_code]);

                    if (match_dist > out->size())
                        return false;
                    size_t start_pos = out->size() - match_dist;
                    for (size_t m = 0; m < match_len; ++m) {
                        out->push_back((*out)[start_pos + m]);
                    }
                }
            }
        } else {
            return false; // Dynamic Huffman or reserved
        }
    }
    return true;
}

} // namespace

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
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        size_t hash = line.find('#');
        if (hash != std::string::npos)
            line = line.substr(0, hash);
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

    uint16_t magic = rd16(sb + 0x38);
    if (magic != 0xEF53) {
        r.failure_note = "ext4 magic not found";
        return r;
    }

    r.is_ext4 = true;
    r.blocks_count = rd32(sb + 0x04);
    uint32_t log_block_size = rd32(sb + 0x18);
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

BinaryAxmlResult parse_binary_axml(const uint8_t* data, size_t size) {
    BinaryAxmlResult res;
    if (!data || size < 8) {
        res.failure_note = "AXML buffer too small";
        return res;
    }

    uint16_t root_type = rd16(data);
    uint32_t root_size = rd32(data + 4);
    if (root_type != 0x0003) {
        res.failure_note = "not binary AXML format";
        return res;
    }

    size_t off = 8;
    size_t limit = (root_size > 0 && root_size <= size) ? root_size : size;
    std::vector<std::string> string_table;

    while (off + 8 <= limit) {
        uint16_t chunk_type = rd16(data + off);
        uint32_t chunk_size = rd32(data + off + 4);
        if (chunk_size < 8 || off + chunk_size > limit)
            break;

        if (chunk_type == 0x0001) { // RES_STRING_POOL_TYPE
            if (chunk_size >= 28) {
                uint32_t string_count = rd32(data + off + 8);
                uint32_t flags = rd32(data + off + 16);
                bool is_utf8 = (flags & 0x0100) != 0;
                uint32_t strings_start = rd32(data + off + 20);
                size_t pool_start_off = off + strings_start;
                size_t offsets_off = off + 28;

                string_table.resize(string_count);
                for (uint32_t i = 0; i < string_count; ++i) {
                    if (offsets_off + i * 4 + 4 > off + chunk_size)
                        break;
                    uint32_t str_rel_off = rd32(data + offsets_off + i * 4);
                    size_t s_off = pool_start_off + str_rel_off;
                    if (s_off >= limit)
                        continue;

                    if (is_utf8) {
                        if (s_off >= limit)
                            continue;
                        uint8_t u16_len = data[s_off++];
                        if (u16_len & 0x80) {
                            if (s_off >= limit)
                                continue;
                            s_off++;
                        }
                        if (s_off >= limit)
                            continue;
                        uint8_t u8_len = data[s_off++];
                        size_t byte_len = u8_len;
                        if (u8_len & 0x80) {
                            if (s_off >= limit)
                                continue;
                            byte_len = ((u8_len & 0x7F) << 8) | data[s_off++];
                        }
                        if (s_off + byte_len <= limit) {
                            string_table[i] =
                                std::string(reinterpret_cast<const char*>(data + s_off), byte_len);
                        }
                    } else { // UTF-16LE
                        if (s_off + 2 > limit)
                            continue;
                        uint16_t char_len = rd16(data + s_off);
                        s_off += 2;
                        if (char_len & 0x8000) {
                            if (s_off + 2 > limit)
                                continue;
                            uint16_t c2 = rd16(data + s_off);
                            s_off += 2;
                            char_len = ((char_len & 0x7FFF) << 16) | c2;
                        }
                        std::string u8_str;
                        u8_str.reserve(char_len);
                        for (size_t c = 0; c < char_len; ++c) {
                            if (s_off + c * 2 + 2 > limit)
                                break;
                            uint16_t code = rd16(data + s_off + c * 2);
                            if (code < 0x80) {
                                u8_str.push_back(static_cast<char>(code));
                            } else if (code < 0x800) {
                                u8_str.push_back(static_cast<char>(0xC0 | (code >> 6)));
                                u8_str.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                            } else {
                                u8_str.push_back(static_cast<char>(0xE0 | (code >> 12)));
                                u8_str.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                                u8_str.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                            }
                        }
                        string_table[i] = u8_str;
                    }
                }
            }
        } else if (chunk_type == 0x0102) { // RES_XML_START_ELEMENT_TYPE
            if (off + 36 <= limit) {
                uint32_t name_idx = rd32(data + off + 20);
                uint16_t attr_start = rd16(data + off + 24);
                uint16_t attr_size = rd16(data + off + 26);
                uint16_t attr_count = rd16(data + off + 28);

                std::string tag_name =
                    (name_idx < string_table.size()) ? string_table[name_idx] : "";
                if (tag_name == "activity") {
                    res.activity_count++;
                    res.ok = true;
                }

                size_t attrs_off = off + 16 + attr_start;
                for (uint16_t a = 0; a < attr_count; ++a) {
                    size_t cur_a_off = attrs_off + a * attr_size;
                    if (cur_a_off + 20 > off + chunk_size || cur_a_off + 20 > limit)
                        break;

                    uint32_t attr_name_idx = rd32(data + cur_a_off + 4);
                    uint32_t attr_raw_val_idx = rd32(data + cur_a_off + 8);
                    uint8_t typed_val_type = data[cur_a_off + 15];
                    uint32_t typed_val_data = rd32(data + cur_a_off + 16);

                    std::string attr_name =
                        (attr_name_idx < string_table.size()) ? string_table[attr_name_idx] : "";
                    std::string val_str;
                    if (attr_raw_val_idx != 0xFFFFFFFFu && attr_raw_val_idx < string_table.size()) {
                        val_str = string_table[attr_raw_val_idx];
                    } else if (typed_val_type == 0x03 && typed_val_data < string_table.size()) {
                        val_str = string_table[typed_val_data];
                    } else {
                        val_str = std::to_string(typed_val_data);
                    }

                    if (tag_name == "manifest") {
                        res.ok = true;
                        if (attr_name == "package") {
                            res.package_name = val_str;
                        } else if (attr_name == "versionCode") {
                            res.version_code = static_cast<int>(typed_val_data);
                        } else if (attr_name == "versionName") {
                            res.version_name = val_str;
                        }
                    } else if (tag_name == "uses-sdk") {
                        res.ok = true;
                        if (attr_name == "minSdkVersion") {
                            res.min_sdk_version = static_cast<int>(typed_val_data);
                        } else if (attr_name == "targetSdkVersion") {
                            res.target_sdk_version = static_cast<int>(typed_val_data);
                        }
                    }
                }
            }
        }

        off += chunk_size;
    }

    if (!res.ok) {
        res.failure_note = "no manifest/uses-sdk element found in AXML";
    }
    return res;
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
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    size_t read_bytes = fread(buf.data(), 1, buf.size(), fp);
    fclose(fp);

    if (read_bytes < 30 || buf[0] != 'P' || buf[1] != 'K' || buf[2] != 0x03 || buf[3] != 0x04) {
        res.failure_note = "invalid ZIP/APK magic";
        return res;
    }

    res.is_apk = true;

    // Iterate ZIP Local File Headers
    size_t pos = 0;
    std::vector<uint8_t> manifest_bytes;
    uint16_t manifest_method = 0;
    uint32_t manifest_uncompressed_size = 0;

    while (pos + 30 <= read_bytes) {
        if (buf[pos] != 'P' || buf[pos + 1] != 'K' || buf[pos + 2] != 0x03 ||
            buf[pos + 3] != 0x04) {
            break;
        }

        uint16_t method = rd16(buf.data() + pos + 8);
        uint32_t comp_size = rd32(buf.data() + pos + 18);
        uint32_t uncomp_size = rd32(buf.data() + pos + 22);
        uint16_t fn_len = rd16(buf.data() + pos + 26);
        uint16_t extra_len = rd16(buf.data() + pos + 28);

        if (pos + 30 + fn_len > read_bytes)
            break;

        std::string fname(reinterpret_cast<const char*>(buf.data() + pos + 30), fn_len);
        res.file_count++;

        if (fname == "AndroidManifest.xml") {
            res.has_manifest = true;
            size_t data_pos = pos + 30 + fn_len + extra_len;
            if (data_pos + comp_size <= read_bytes) {
                manifest_bytes.assign(buf.data() + data_pos, buf.data() + data_pos + comp_size);
                manifest_method = method;
                manifest_uncompressed_size = uncomp_size;
            }
        } else if (fname == "classes.dex") {
            res.has_classes_dex = true;
        } else if (fname.rfind("lib/", 0) == 0 && fname.find(".so") != std::string::npos) {
            res.has_native_libs = true;
        }

        size_t next_pos = pos + 30 + fn_len + extra_len + comp_size;
        if (next_pos <= pos)
            break;
        pos = next_pos;
    }

    // Try parsing binary AXML from AndroidManifest.xml
    if (res.has_manifest && !manifest_bytes.empty()) {
        std::vector<uint8_t> decompressed;
        const uint8_t* axml_ptr = nullptr;
        size_t axml_sz = 0;

        if (manifest_method == 0) { // STORE
            axml_ptr = manifest_bytes.data();
            axml_sz = manifest_bytes.size();
        } else if (manifest_method == 8) { // DEFLATE
            if (inflate_deflate(manifest_bytes.data(), manifest_bytes.size(), &decompressed,
                                manifest_uncompressed_size)) {
                axml_ptr = decompressed.data();
                axml_sz = decompressed.size();
            }
        }

        if (axml_ptr && axml_sz >= 8) {
            BinaryAxmlResult axml = parse_binary_axml(axml_ptr, axml_sz);
            if (axml.ok) {
                res.package_name = axml.package_name;
                res.min_sdk_version = axml.min_sdk_version;
                res.target_sdk_version = axml.target_sdk_version;
                res.version_code = axml.version_code;
                res.version_name = axml.version_name;
                res.activity_count = axml.activity_count;
                res.package_note = "parsed binary AXML from AndroidManifest.xml";
            } else {
                res.package_note =
                    "AndroidManifest.xml present; AXML parse note: " + axml.failure_note;
            }
        } else {
            res.package_note =
                "AndroidManifest.xml compressed with unhandled method or inflate failed";
        }
    } else if (res.has_manifest) {
        res.package_note = "AndroidManifest.xml header present";
    }

    return res;
}

BuildProp parse_build_prop(const std::string& text) {
    BuildProp prop;
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

    for (std::string line : lines) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        size_t hash = line.find('#');
        if (hash != std::string::npos)
            line = line.substr(0, hash);
        size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos)
            continue;
        size_t e = line.find_last_not_of(" \t");
        line = line.substr(b, e - b + 1);

        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        size_t kb = key.find_first_not_of(" \t");
        if (kb != std::string::npos) {
            size_t ke = key.find_last_not_of(" \t");
            key = key.substr(kb, ke - kb + 1);
        }
        size_t vb = val.find_first_not_of(" \t");
        if (vb != std::string::npos) {
            size_t ve = val.find_last_not_of(" \t");
            val = val.substr(vb, ve - vb + 1);
        } else {
            val.clear();
        }

        prop.all_properties.emplace_back(key, val);

        if (key == "ro.build.version.release") {
            prop.release_version = val;
        } else if (key == "ro.build.version.sdk") {
            prop.sdk_version = atoi(val.c_str());
        } else if (key == "ro.product.model") {
            prop.model = val;
        } else if (key == "ro.product.brand") {
            prop.brand = val;
        } else if (key == "ro.build.display.id") {
            prop.build_id = val;
        }
    }

    return prop;
}

} // namespace android
} // namespace ps5droid
