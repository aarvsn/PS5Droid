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
    std::vector<std::string> strings = {"manifest",      "package",          pkg_name,  "uses-sdk",
                                        "minSdkVersion", "targetSdkVersion", "activity"};
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

    ps5droid::platform::write_file(path, zip.data(), zip.size());

    ApkInspection apk = inspect_apk(path);
    CHECK_MSG(apk.is_apk, "APK detection failed");
    CHECK_MSG(apk.has_manifest, "manifest entry must be detected");
    CHECK_MSG(apk.package_name == "org.ps5droid.sample", "package name should be parsed from AXML");
    CHECK_MSG(apk.min_sdk_version == 21, "minSdkVersion mismatch");
    CHECK_MSG(apk.target_sdk_version == 30, "targetSdkVersion mismatch");
    CHECK_MSG(apk.activity_count == 1, "activity count mismatch");

    ps5droid::platform::remove_file(path);
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
