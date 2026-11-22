// PS5Droid - Milestone 5 & 6 unit tests.
#include "test_framework.h"

#include <cstring>
#include <vector>

#include "android/binder/binder_engine.h"
#include "android/image_manager.h"
#include "platform/platform.h"

using namespace ps5droid::android;

namespace {
std::vector<uint8_t> echo_handler(uint32_t code, const std::vector<uint8_t>& in_data, void*) {
    (void)code;
    return in_data;
}

// Helper to construct a valid binary AXML buffer containing manifest + uses-sdk + activity.
std::vector<uint8_t> create_synthetic_axml(const std::string& pkg_name, int min_sdk,
                                           int target_sdk) {
    std::vector<uint8_t> data;

    auto push16 = [&](uint16_t v) {
        data.push_back(static_cast<uint8_t>(v & 0xFF));
        data.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    };

    auto push32 = [&](uint32_t v) {
        data.push_back(static_cast<uint8_t>(v & 0xFF));
        data.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        data.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        data.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    };

    auto push_utf8_str = [&](const std::string& str, std::vector<uint8_t>& pool) {
        size_t off = pool.size();
        pool.push_back(static_cast<uint8_t>(str.size())); // UTF-16 len
        pool.push_back(static_cast<uint8_t>(str.size())); // UTF-8 len
        pool.insert(pool.end(), str.begin(), str.end());
        pool.push_back(0); // NUL
        return static_cast<uint32_t>(off);
    };

    // Root header: RES_XML_TYPE (0x0003)
    push16(0x0003);
    push16(8);
    push32(0); // placeholder for root size

    // String pool strings
    std::vector<std::string> strings = {"manifest",
                                        "package",
                                        pkg_name,
                                        "uses-sdk",
                                        "minSdkVersion",
                                        "targetSdkVersion",
                                        "activity",
                                        "application",
                                        "label",
                                        "TestApp",
                                        "uses-permission",
                                        "name",
                                        "android.permission.INTERNET"};
    std::vector<uint32_t> offsets;
    std::vector<uint8_t> str_bytes;

    for (const auto& s : strings) {
        offsets.push_back(push_utf8_str(s, str_bytes));
    }

    push16(0x0001); // RES_STRING_POOL_TYPE
    push16(28);     // header size
    uint32_t pool_chunk_size = 28 + static_cast<uint32_t>(offsets.size() * 4 + str_bytes.size());
    push32(pool_chunk_size);
    push32(static_cast<uint32_t>(strings.size()));
    push32(0);      // style count
    push32(0x0100); // UTF-8 flag
    uint32_t strings_start = 28 + static_cast<uint32_t>(offsets.size() * 4);
    push32(strings_start);
    push32(0); // styles start

    for (uint32_t off : offsets) {
        push32(off);
    }
    data.insert(data.end(), str_bytes.begin(), str_bytes.end());

    // Start Element: <manifest package=pkg_name>
    push16(0x0102); // RES_XML_START_ELEMENT_TYPE
    push16(16);
    uint32_t manifest_size = 36 + 20; // 36 header + 1 attr (20 bytes)
    push32(manifest_size);
    push32(1);           // line number
    push32(0xFFFFFFFFu); // comment
    push32(0xFFFFFFFFu); // ns
    push32(0);           // name_idx = "manifest"
    push16(20);          // attr_start
    push16(20);          // attr_size
    push16(1);           // attr_count
    push16(0);           // idIndex
    push16(0);           // classIndex
    push16(0);           // styleIndex
    // Attr 0: package=pkg_name
    push32(0xFFFFFFFFu);  // ns
    push32(1);            // name_idx = "package"
    push32(2);            // raw_val_idx = pkg_name
    push16(8);            // typed_val_size
    data.push_back(0);    // res0
    data.push_back(0x03); // type = TYPE_STRING
    push32(2);            // val_data = string 2

    // Start Element: <uses-sdk minSdkVersion=min_sdk targetSdkVersion=target_sdk>
    push16(0x0102);
    push16(16);
    push32(36 + 40); // 2 attrs
    push32(2);
    push32(0xFFFFFFFFu);
    push32(0xFFFFFFFFu);
    push32(3); // "uses-sdk"
    push16(20);
    push16(20);
    push16(2); // 2 attrs
    push16(0);
    push16(0);
    push16(0);
    // Attr 0: minSdkVersion
    push32(0xFFFFFFFFu);
    push32(4); // "minSdkVersion"
    push32(0xFFFFFFFFu);
    push16(8);
    data.push_back(0);
    data.push_back(0x10); // TYPE_INT_DEC
    push32(static_cast<uint32_t>(min_sdk));
    // Attr 1: targetSdkVersion
    push32(0xFFFFFFFFu);
    push32(5); // "targetSdkVersion"
    push32(0xFFFFFFFFu);
    push16(8);
    data.push_back(0);
    data.push_back(0x10);
    push32(static_cast<uint32_t>(target_sdk));

    // Start Element: <activity>
    push16(0x0102);
    push16(16);
    push32(36);
    push32(3);
    push32(0xFFFFFFFFu);
    push32(0xFFFFFFFFu);
    push32(6); // "activity"
    push16(20);
    push16(20);
    push16(0);
    push16(0);
    push16(0);
    push16(0);

    // Start Element: <application label="TestApp">
    push16(0x0102);
    push16(16);
    push32(36 + 20); // 1 attr
    push32(4);
    push32(0xFFFFFFFFu);
    push32(0xFFFFFFFFu);
    push32(7); // "application"
    push16(20);
    push16(20);
    push16(1); // 1 attr
    push16(0);
    push16(0);
    push16(0);
    // Attr 0: label="TestApp"
    push32(0xFFFFFFFFu);
    push32(8); // "label"
    push32(9); // "TestApp"
    push16(8);
    data.push_back(0);
    data.push_back(0x03); // TYPE_STRING
    push32(9);            // "TestApp"

    // Start Element: <uses-permission name="android.permission.INTERNET">
    push16(0x0102);
    push16(16);
    push32(36 + 20); // 1 attr
    push32(5);
    push32(0xFFFFFFFFu);
    push32(0xFFFFFFFFu);
    push32(10); // "uses-permission"
    push16(20);
    push16(20);
    push16(1); // 1 attr
    push16(0);
    push16(0);
    push16(0);
    // Attr 0: name="android.permission.INTERNET"
    push32(0xFFFFFFFFu);
    push32(11); // "name"
    push32(12); // "android.permission.INTERNET"
    push16(8);
    data.push_back(0);
    data.push_back(0x03); // TYPE_STRING
    push32(12);

    // Patch root size
    uint32_t total = static_cast<uint32_t>(data.size());
    data[4] = static_cast<uint8_t>(total & 0xFF);
    data[5] = static_cast<uint8_t>((total >> 8) & 0xFF);
    data[6] = static_cast<uint8_t>((total >> 16) & 0xFF);
    data[7] = static_cast<uint8_t>((total >> 24) & 0xFF);

    return data;
}

} // namespace

PS5DROID_TEST(binder_engine_registration_and_tx) {
    BinderEngine engine;
    uint32_t h = engine.register_service("test_service", echo_handler);
    CHECK_MSG(h > 0, "service registration failed");
    CHECK_MSG(engine.lookup_service("test_service") == h, "lookup failed");

    std::vector<uint8_t> in = {'P', 'S', '5'};
    std::vector<uint8_t> out;
    bool ok = engine.transact(h, 1, in, &out);
    CHECK_MSG(ok, "transaction failed");
    CHECK_MSG(out == in, "reply mismatch");
}

PS5DROID_TEST(binary_axml_parsing) {
    auto axml_buf = create_synthetic_axml("com.example.ps5droid.app", 26, 31);
    BinaryAxmlResult res = parse_binary_axml(axml_buf.data(), axml_buf.size());

    CHECK_MSG(res.ok, "AXML parsing must succeed");
    CHECK_MSG(res.package_name == "com.example.ps5droid.app", "package name mismatch");
    CHECK_MSG(res.min_sdk_version == 26, "minSdkVersion mismatch");
    CHECK_MSG(res.target_sdk_version == 31, "targetSdkVersion mismatch");
    CHECK_MSG(res.activity_count == 1, "activity count mismatch");
}

PS5DROID_TEST(apk_inspection_basic) {
    std::string path = "test_app.apk";

    // Create an uncompressed (STORE) ZIP containing AndroidManifest.xml with synthetic AXML
    auto axml_data = create_synthetic_axml("org.ps5droid.sample", 21, 30);

    std::vector<uint8_t> zip;
    auto push16 = [&](uint16_t v) {
        zip.push_back(static_cast<uint8_t>(v & 0xFF));
        zip.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    };
    auto push32 = [&](uint32_t v) {
        zip.push_back(static_cast<uint8_t>(v & 0xFF));
        zip.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        zip.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        zip.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    };

    // LFH for AndroidManifest.xml
    push32(0x04034b50);                              // PK\x03\x04
    push16(20);                                      // ver
    push16(0);                                       // flags
    push16(0);                                       // STORE
    push16(0);                                       // time
    push16(0);                                       // date
    push32(0);                                       // crc
    push32(static_cast<uint32_t>(axml_data.size())); // comp_size
    push32(static_cast<uint32_t>(axml_data.size())); // uncomp_size
    const std::string fn = "AndroidManifest.xml";
    push16(static_cast<uint16_t>(fn.size()));
    push16(0); // extra len
    zip.insert(zip.end(), fn.begin(), fn.end());
    zip.insert(zip.end(), axml_data.begin(), axml_data.end());

    // LFH for lib/x86_64/libnative.so
    const std::string so_fn = "lib/x86_64/libnative.so";
    const std::string so_data = "dummy so payload";
    push32(0x04034b50);
    push16(20);
    push16(0);
    push16(0);
    push16(0);
    push16(0);
    push32(0);
    push32(static_cast<uint32_t>(so_data.size()));
    push32(static_cast<uint32_t>(so_data.size()));
    push16(static_cast<uint16_t>(so_fn.size()));
    push16(0);
    zip.insert(zip.end(), so_fn.begin(), so_fn.end());
    zip.insert(zip.end(), so_data.begin(), so_data.end());

    ps5droid::platform::write_file(path, zip.data(), zip.size());

    ApkInspection apk = inspect_apk(path);
    CHECK_MSG(apk.is_apk, "APK detection failed");
    CHECK_MSG(apk.has_manifest, "manifest entry must be detected");
    CHECK_MSG(apk.has_native_libs, "native libs should be detected");
    CHECK_MSG(apk.native_abis.size() == 1 && apk.native_abis[0] == "x86_64",
              "native ABI x86_64 should be extracted");
    CHECK_MSG(apk.package_name == "org.ps5droid.sample", "package name should be parsed from AXML");
    CHECK_MSG(apk.min_sdk_version == 21, "minSdkVersion mismatch");
    CHECK_MSG(apk.target_sdk_version == 30, "targetSdkVersion mismatch");
    CHECK_MSG(apk.app_label == "TestApp", "app_label mismatch");
    CHECK_MSG(apk.permissions.size() == 1 && apk.permissions[0] == "android.permission.INTERNET",
              "permission mismatch");
    CHECK_MSG(apk.activity_count == 1, "activity count mismatch");

    ps5droid::platform::remove_file(path);
}

PS5DROID_TEST(apk_inspection_deflated_manifest) {
    // AndroidManifest.xml compressed with a DYNAMIC Huffman block (zlib
    // level 6, wbits=-15). Real APKs use this block type; v1.0.5 could not
    // read them.
    static const uint8_t kCompressedManifest[] = {
        237, 150, 193, 74,  195, 64,  16,  134, 255, 164, 105, 154, 26,  67,  114, 168, 32,  212,
        139, 224, 81,  115, 19,  189, 10,  22,  17,  164, 136, 22,  15,  222, 214, 100, 45,  75,
        154, 100, 201, 70,  161, 120, 237, 51,  120, 243, 89,  117, 182, 93,  105, 136, 250, 2,
        146, 9,   31,  187, 204, 254, 255, 36,  153, 203, 108, 15,  30,  194, 33,  96,  225, 0,
        190, 5,   4,   48,  65,  251, 91,  108, 195, 39,  246, 136, 99,  226, 140, 184, 34,  30,
        9,   78,  40,  226, 141, 88,  17,  239, 196, 7,   225, 121, 57,  43,  196, 51,  87,  53,
        6,   3,   201, 146, 140, 205, 57,  70,  163, 178, 154, 199, 82,  157, 166, 85,  41,  210,
        56,  93,  22,  44,  23,  9,   105, 95,  20,  87,  39,  42,  205, 16,  4,   185, 40,  238,
        211, 236, 129, 87,  74,  148, 5,   162, 168, 102, 213, 156, 215, 141, 148, 231, 177, 164,
        22,  175, 162, 94,  194, 247, 153, 148, 11,  145, 176, 90,  31,  244, 251, 11,  246, 196,
        23,  112, 221, 203, 101, 113, 33,  37,  194, 112, 93,  87,  242, 42,  23,  106, 237, 117,
        28,  122, 35,  199, 120, 204, 138, 205, 39,  108, 207, 226, 235, 233, 108, 114, 55,  157,
        204, 96,  91,  17,  206, 55,  29,  192, 167, 9,   221, 130, 17,  61,  150, 105, 135, 206,
        233, 189, 173, 127, 21,  232, 233, 85,  251, 110, 90,  190, 158, 241, 217, 13,  159, 99,
        86,  242, 69,  251, 102, 223, 111, 228, 14,  77,  173, 163, 86,  45,  215, 212, 250, 142,
        78,  211, 105, 58,  77,  167, 233, 52,  157, 230, 63,  107, 218, 179, 120, 128, 159, 179,
        152, 230, 38,  232, 22,  179, 158, 197, 195, 63,  124, 59,  191, 248, 244, 181, 102, 215,
        248, 244, 250, 5};
    const uint32_t kUncompressedSize = 2319;

    CHECK_MSG(((kCompressedManifest[0] >> 1) & 3) == 2, "fixture must use a dynamic Huffman block");

    std::vector<uint8_t> zip;
    auto push16 = [&](uint16_t v) {
        zip.push_back(static_cast<uint8_t>(v & 0xFF));
        zip.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    };
    auto push32 = [&](uint32_t v) {
        zip.push_back(static_cast<uint8_t>(v & 0xFF));
        zip.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        zip.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        zip.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    };

    push32(0x04034b50);
    push16(20);
    push16(0);
    push16(8); // DEFLATE
    push16(0);
    push16(0);
    push32(0);
    push32(static_cast<uint32_t>(sizeof(kCompressedManifest)));
    push32(kUncompressedSize);
    const std::string fn = "AndroidManifest.xml";
    push16(static_cast<uint16_t>(fn.size()));
    push16(0);
    zip.insert(zip.end(), fn.begin(), fn.end());
    zip.insert(zip.end(), kCompressedManifest, kCompressedManifest + sizeof(kCompressedManifest));

    const std::string dex = "classes.dex";
    const std::string dex_data = "dex\n035\0";
    push32(0x04034b50);
    push16(20);
    push16(0);
    push16(0);
    push16(0);
    push16(0);
    push32(0);
    push32(static_cast<uint32_t>(dex_data.size()));
    push32(static_cast<uint32_t>(dex_data.size()));
    push16(static_cast<uint16_t>(dex.size()));
    push16(0);
    zip.insert(zip.end(), dex.begin(), dex.end());
    zip.insert(zip.end(), dex_data.begin(), dex_data.end());

    std::string path = "test_dynamic_manifest.apk";
    CHECK_MSG(ps5droid::platform::write_file(path, zip.data(), zip.size()), "fixture write failed");

    ApkInspection apk = inspect_apk(path);
    CHECK_MSG(apk.is_apk, "APK detection failed");
    CHECK_MSG(apk.has_manifest, "manifest entry must be detected");
    CHECK_MSG(apk.has_classes_dex, "classes.dex must be detected");
    CHECK_MSG(apk.package_name == "org.ps5droid.dynamic", "dynamic manifest package mismatch");
    CHECK_MSG(apk.min_sdk_version == 24, "dynamic manifest minSdk mismatch");
    CHECK_MSG(apk.target_sdk_version == 33, "dynamic manifest targetSdk mismatch");
    CHECK_MSG(apk.activity_count == 50, "dynamic manifest activity count mismatch");
    CHECK_MSG(apk.app_label == "DynApp", "dynamic manifest label mismatch");
    CHECK_MSG(apk.permissions.size() == 1 && apk.permissions[0] == "android.permission.INTERNET",
              "dynamic manifest permission mismatch");
    CHECK_MSG(apk.package_note.find("parsed binary AXML") != std::string::npos,
              "package note must report AXML success");

    ps5droid::platform::remove_file(path);
}

PS5DROID_TEST(deflate_dynamic_huffman_block) {
    // Raw DEFLATE stream produced by zlib level 6 (wbits=-15) for the
    // payload reconstructed below. Its first block is BTYPE=2 (dynamic
    // Huffman), which the v1.0.5 decoder rejected.
    static const uint8_t kDynamic[] = {
        237, 214, 61,  14,  130, 64,  16,  134, 225, 171, 16,  122, 151, 202, 198, 172, 116,
        20,  22,  82,  24,  46,  48,  129, 149, 76,  12,  11,  97,  209, 132, 219, 75,  252,
        137, 71,  96,  13,  111, 253, 204, 219, 78,  62,  219, 137, 215, 171, 11,  83,  50,
        72,  125, 147, 214, 29,  211, 126, 108, 205, 16,  246, 205, 216, 107, 99,  154, 217,
        75,  167, 117, 154, 91,  169, 39,  125, 232, 52,  39,  226, 95,  116, 88,  96,  185,
        62,  139, 250, 52,  131, 97,  24,  254, 67,  190, 7,   23,  118, 131, 27,  59,  13,
        65,  123, 159, 188, 245, 115, 107, 126, 96,  78,  101, 85,  92,  202, 162, 34,  34,
        34,  218, 84,  148, 125, 87,  82,  110, 217, 75,  48,  12,  111, 150, 35,  127, 213,
        68,  68,  68,  236, 37,  24,  134, 225, 213, 57,  242, 87,  77,  68,  68,  196, 94,
        130, 97,  24,  94,  157, 35,  127, 213, 68,  68,  68,  209, 236, 165, 39};

    std::string payload;
    const std::string manifest = "<manifest package=\"org.ps5droid.dynamic\">";
    const std::string activity = "<activity android:name=\"Main\"/>";
    const std::string permission = "<uses-permission name=\"android.permission.INTERNET\"/>";
    for (int i = 0; i < 4; ++i) {
        payload += manifest;
        for (int j = 0; j < 40; ++j)
            payload += activity;
        for (int j = 0; j < 20; ++j)
            payload += permission;
        payload += "</manifest>";
    }

    CHECK_MSG(((kDynamic[0] >> 1) & 3) == 2, "test vector must be a dynamic block");

    std::vector<uint8_t> out;
    CHECK_MSG(inflate_deflate(kDynamic, sizeof(kDynamic), &out, payload.size()),
              "dynamic Huffman inflate failed");
    CHECK_MSG(out.size() == payload.size(), "inflated size mismatch");
    CHECK_MSG(memcmp(out.data(), payload.data(), payload.size()) == 0, "inflated bytes mismatch");

    // Truncated input must fail cleanly instead of reading past the buffer.
    std::vector<uint8_t> truncated;
    CHECK_MSG(!inflate_deflate(kDynamic, 40, &truncated, payload.size()),
              "truncated dynamic stream must be rejected");
}

PS5DROID_TEST(build_prop_parsing) {
    std::string content = "# Android build properties\n"
                          "ro.build.version.release=11\n"
                          "ro.build.version.sdk=30\n"
                          "ro.product.model=PlayStation 5\n"
                          "ro.product.brand=Sony\n"
                          "ro.build.display.id=PS5Droid-1.0.3\n";
    BuildProp prop = parse_build_prop(content);
    CHECK_MSG(prop.release_version == "11", "release version mismatch");
    CHECK_MSG(prop.sdk_version == 30, "sdk version mismatch");
    CHECK_MSG(prop.model == "PlayStation 5", "model mismatch");
    CHECK_MSG(prop.brand == "Sony", "brand mismatch");
    CHECK_MSG(prop.build_id == "PS5Droid-1.0.3", "build_id mismatch");
    CHECK_MSG(prop.all_properties.size() == 5, "all properties count mismatch");
}
