// PS5Droid - filesystem platform implementation.
//
// Deliberately implemented with C stdio/dirent rather than std::filesystem:
// the ps5-payload-sdk libc provides stdio and dirent (verified headers
// include/freebsd/sys/dirent.h), while std::filesystem support in the SDK's
// libc++ build is unverified.
#include "platform/platform.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace ps5droid {
namespace platform {

bool read_file(const std::string& path, std::string* out) {
    if (out == nullptr)
        return false;
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr)
        return false;
    std::string data;
    char buf[65536];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        data.append(buf, n);
    }
    if (ferror(f))
        ok = false;
    fclose(f);
    if (ok)
        *out = std::move(data);
    return ok;
}

bool write_file(const std::string& path, const void* data, size_t len) {
    FILE* f = fopen(path.c_str(), "wb");
    if (f == nullptr)
        return false;
    bool ok = fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0)
        ok = false;
    return ok;
}

bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

bool is_directory(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool remove_file(const std::string& path) {
    return remove(path.c_str()) == 0;
}

bool make_dir(const std::string& path, int mode) {
    if (mkdir(path.c_str(), static_cast<mode_t>(mode)) == 0)
        return true;
    return errno == EEXIST; // already exists is a success for callers
}

std::vector<std::string> list_dir(const std::string& path) {
    std::vector<std::string> out;
    DIR* d = opendir(path.c_str());
    if (d == nullptr)
        return out;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..")
            continue;
        out.push_back(std::move(name));
    }
    closedir(d);
    return out;
}

} // namespace platform
} // namespace ps5droid
